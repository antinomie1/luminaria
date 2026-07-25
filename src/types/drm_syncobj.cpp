#include "luminaria/drm_syncobj.hpp"

#include <cstdint>
#include <ctime>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <xf86drm.h>

#include "linux-drm-syncobj-v1-protocol.h"

#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"

namespace luminaria {

struct DrmSyncobjManager::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    int drm_fd = -1;
    unsigned acquire_timeout_ms = 50;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
        if (drm_fd >= 0) {
            close(drm_fd);
        }
    }
};

namespace {

using Mgr = DrmSyncobjManager::Impl;

// A client timeline syncobj imported into our render node. Owned by its
// wp_linux_drm_syncobj_timeline_v1 resource.
struct Timeline {
    Mgr* mgr = nullptr;
    uint32_t handle = 0;
};

Timeline* timeline_of(wl_resource* r) {
    return static_cast<Timeline*>(wl_resource_get_user_data(r));
}

void timeline_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
constexpr struct wp_linux_drm_syncobj_timeline_v1_interface timeline_impl = {
    .destroy = timeline_destroy_request,
};
void timeline_resource_destroy(wl_resource* resource) {
    Timeline* timeline = timeline_of(resource);
    if (timeline->mgr != nullptr && timeline->mgr->drm_fd >= 0) {
        drmSyncobjDestroy(timeline->mgr->drm_fd, timeline->handle);
    }
    delete timeline;
}

// Per-surface synchronisation state. Owned by its
// wp_linux_drm_syncobj_surface_v1 resource; the Surface may die first.
struct SyncSurface {
    Mgr* mgr = nullptr;
    Surface* surface = nullptr;
    Signal<SurfaceCommit>::Connection commit_conn;
    Signal<SurfaceRendered>::Connection rendered_conn;
    Signal<SurfaceDestroy>::Connection destroy_conn;

    // Points set since the last commit (pending surface state).
    bool has_pending_acquire = false;
    uint32_t pending_acquire_handle = 0;
    uint64_t pending_acquire_point = 0;
    bool has_pending_release = false;
    uint32_t pending_release_handle = 0;
    uint64_t pending_release_point = 0;

    // The release point belonging to the buffer currently on screen.
    bool has_current_release = false;
    uint32_t current_release_handle = 0;
    uint64_t current_release_point = 0;
};

SyncSurface* sync_surface_of(wl_resource* r) {
    return static_cast<SyncSurface*>(wl_resource_get_user_data(r));
}

uint64_t point_from(uint32_t hi, uint32_t lo) {
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

void signal_release(SyncSurface* sync) {
    if (!sync->has_current_release || sync->mgr == nullptr || sync->mgr->drm_fd < 0) {
        return;
    }
    uint32_t handle = sync->current_release_handle;
    uint64_t point = sync->current_release_point;
    drmSyncobjTimelineSignal(sync->mgr->drm_fd, &handle, &point, 1);
    sync->has_current_release = false;
}

/// Pull a timeline point out as a sync_file fd. This is the whole point of
/// explicit sync: the fence travels to the renderer (and on to KMS) as a plain
/// fd, and nobody waits on the CPU for the client's GPU work to finish.
/// Returns -1 if the driver cannot hand it over.
int export_point(int drm_fd, uint32_t handle, uint64_t point) {
    uint32_t tmp = 0;
    if (drmSyncobjCreate(drm_fd, 0, &tmp) != 0) {
        return -1;
    }
    int sync_fd = -1;
    if (drmSyncobjTransfer(drm_fd, tmp, 0, handle, point, 0) != 0 ||
        drmSyncobjExportSyncFile(drm_fd, tmp, &sync_fd) != 0) {
        sync_fd = -1;
    }
    drmSyncobjDestroy(drm_fd, tmp);
    return sync_fd;
}

/// Make `point` signal when `sync_file_fd` does, instead of right now.
bool import_into_point(int drm_fd, uint32_t handle, uint64_t point, int sync_file_fd) {
    uint32_t tmp = 0;
    if (drmSyncobjCreate(drm_fd, 0, &tmp) != 0) {
        return false;
    }
    const bool done = drmSyncobjImportSyncFile(drm_fd, tmp, sync_file_fd) == 0 &&
                      drmSyncobjTransfer(drm_fd, handle, point, tmp, 0, 0) == 0;
    drmSyncobjDestroy(drm_fd, tmp);
    return done;
}

/// A client is supposed to have submitted its work before committing, so the
/// acquire fence exists and export_point() succeeds. If it hasn't, the fence has
/// not materialised yet and there is nothing to export — wait, bounded, for it
/// to appear and try once more. A client that never signals costs one late
/// frame, not a hung compositor.
int export_acquire(SyncSurface* sync) {
    const int drm_fd = sync->mgr->drm_fd;
    const uint32_t handle = sync->pending_acquire_handle;
    const uint64_t point = sync->pending_acquire_point;
    if (int fd = export_point(drm_fd, handle, point); fd >= 0) {
        return fd;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t timeout_ns = static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec +
                               static_cast<int64_t>(sync->mgr->acquire_timeout_ms) * 1000000;
    uint32_t h = handle;
    uint64_t p = point;
    drmSyncobjTimelineWait(drm_fd, &h, &p, 1, timeout_ns,
                           DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
                               DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
                           nullptr);
    return export_point(drm_fd, handle, point);
}

void on_commit(SyncSurface* sync) {
    if (sync->has_pending_acquire && sync->surface != nullptr && sync->mgr != nullptr &&
        sync->mgr->drm_fd >= 0) {
        // The renderer picks this up and waits on it as a VkSemaphore.
        sync->surface->set_acquire_fence(export_acquire(sync));
    }
    // The buffer that was current until now is being replaced: release it. If
    // the compositor told us when it finished reading (on_rendered below), this
    // already happened and the point signals on the GPU's own schedule.
    signal_release(sync);
    if (sync->has_pending_release) {
        sync->has_current_release = true;
        sync->current_release_handle = sync->pending_release_handle;
        sync->current_release_point = sync->pending_release_point;
    }
    sync->has_pending_acquire = false;
    sync->has_pending_release = false;
}

/// The compositor submitted the render that samples this surface. Rather than
/// signalling the release point once the frame is done, hand the client the
/// render's own fence: it can start drawing into the buffer the moment the GPU
/// stops reading it, without a round trip through us.
void on_rendered(SyncSurface* sync, int fence_fd) {
    if (!sync->has_current_release || sync->mgr == nullptr || sync->mgr->drm_fd < 0) {
        return;
    }
    if (fence_fd >= 0 && import_into_point(sync->mgr->drm_fd, sync->current_release_handle,
                                           sync->current_release_point, fence_fd)) {
        sync->has_current_release = false; // the fence carries the signal now
        return;
    }
    signal_release(sync); // no usable fence: the work is done, say so
}

void sync_surface_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void sync_surface_set_acquire_point(wl_client*, wl_resource* resource, wl_resource* timeline,
                                    uint32_t point_hi, uint32_t point_lo) {
    SyncSurface* sync = sync_surface_of(resource);
    if (sync->surface == nullptr) {
        wl_resource_post_error(resource, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE,
                               "the wl_surface is gone");
        return;
    }
    sync->has_pending_acquire = true;
    sync->pending_acquire_handle = timeline_of(timeline)->handle;
    sync->pending_acquire_point = point_from(point_hi, point_lo);
}
void sync_surface_set_release_point(wl_client*, wl_resource* resource, wl_resource* timeline,
                                    uint32_t point_hi, uint32_t point_lo) {
    SyncSurface* sync = sync_surface_of(resource);
    if (sync->surface == nullptr) {
        wl_resource_post_error(resource, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE,
                               "the wl_surface is gone");
        return;
    }
    const uint64_t point = point_from(point_hi, point_lo);
    if (sync->has_pending_acquire && sync->pending_acquire_handle == timeline_of(timeline)->handle &&
        sync->pending_acquire_point == point) {
        wl_resource_post_error(resource,
                               WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS,
                               "acquire and release point are identical");
        return;
    }
    sync->has_pending_release = true;
    sync->pending_release_handle = timeline_of(timeline)->handle;
    sync->pending_release_point = point;
}
constexpr struct wp_linux_drm_syncobj_surface_v1_interface sync_surface_impl = {
    .destroy = sync_surface_destroy_request,
    .set_acquire_point = sync_surface_set_acquire_point,
    .set_release_point = sync_surface_set_release_point,
};
void sync_surface_resource_destroy(wl_resource* resource) {
    SyncSurface* sync = sync_surface_of(resource);
    // Don't strand the client waiting on a release that will never come.
    signal_release(sync);
    delete sync;
}

// ---- manager ----
void manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void manager_get_surface(wl_client* client, wl_resource* resource, uint32_t id,
                         wl_resource* surface_resource) {
    auto* mgr = static_cast<Mgr*>(wl_resource_get_user_data(resource));
    auto* surface = static_cast<Surface*>(wl_resource_get_user_data(surface_resource));

    wl_resource* sync_resource =
        wl_resource_create(client, &wp_linux_drm_syncobj_surface_v1_interface,
                           wl_resource_get_version(resource), id);
    if (sync_resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* sync = new SyncSurface{};
    sync->mgr = mgr;
    sync->surface = surface;
    sync->commit_conn = surface->commit.connect([sync](SurfaceCommit&) { on_commit(sync); });
    sync->rendered_conn =
        surface->rendered.connect([sync](SurfaceRendered& e) { on_rendered(sync, e.fence_fd); });
    sync->destroy_conn = surface->destroy.connect([sync](SurfaceDestroy&) {
        signal_release(sync);
        sync->surface = nullptr;
        sync->commit_conn.disconnect();
        sync->rendered_conn.disconnect();
    });
    wl_resource_set_implementation(sync_resource, &sync_surface_impl, sync,
                                   sync_surface_resource_destroy);
}

void manager_import_timeline(wl_client* client, wl_resource* resource, uint32_t id, int32_t fd) {
    auto* mgr = static_cast<Mgr*>(wl_resource_get_user_data(resource));
    uint32_t handle = 0;
    const int ret = drmSyncobjFDToHandle(mgr->drm_fd, fd, &handle);
    close(fd);
    if (ret != 0) {
        wl_resource_post_error(resource, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE,
                               "the fd is not a DRM timeline syncobj");
        return;
    }
    wl_resource* timeline_resource =
        wl_resource_create(client, &wp_linux_drm_syncobj_timeline_v1_interface,
                           wl_resource_get_version(resource), id);
    if (timeline_resource == nullptr) {
        drmSyncobjDestroy(mgr->drm_fd, handle);
        wl_client_post_no_memory(client);
        return;
    }
    auto* timeline = new Timeline{mgr, handle};
    wl_resource_set_implementation(timeline_resource, &timeline_impl, timeline,
                                   timeline_resource_destroy);
}

constexpr struct wp_linux_drm_syncobj_manager_v1_interface manager_impl = {
    .destroy = manager_destroy_request,
    .get_surface = manager_get_surface,
    .import_timeline = manager_import_timeline,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource =
        wl_resource_create(client, &wp_linux_drm_syncobj_manager_v1_interface,
                           static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

/// A render node whose kernel driver supports timeline syncobjs.
int open_syncobj_render_node() {
    for (int i = 128; i < 128 + 16; ++i) {
        const std::string path = "/dev/dri/renderD" + std::to_string(i);
        int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        uint64_t cap = 0;
        if (drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) == 0 && cap != 0) {
            return fd;
        }
        close(fd);
    }
    return -1;
}

} // namespace

DrmSyncobjManager::DrmSyncobjManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
DrmSyncobjManager::~DrmSyncobjManager() = default;
DrmSyncobjManager::DrmSyncobjManager(DrmSyncobjManager&&) noexcept = default;
DrmSyncobjManager& DrmSyncobjManager::operator=(DrmSyncobjManager&&) noexcept = default;

Result<DrmSyncobjManager> DrmSyncobjManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->drm_fd = open_syncobj_render_node();
    if (impl->drm_fd < 0) {
        return fail("linux-drm-syncobj: no render node with timeline syncobj support");
    }
    impl->global = wl_global_create(impl->display, &wp_linux_drm_syncobj_manager_v1_interface, 1,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wp_linux_drm_syncobj_manager_v1) failed");
    }
    return DrmSyncobjManager{std::move(impl)};
}

void DrmSyncobjManager::set_acquire_timeout_ms(unsigned ms) noexcept {
    impl_->acquire_timeout_ms = ms;
}

} // namespace luminaria
