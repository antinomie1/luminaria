// linux-drm-syncobj-v1: explicit GPU synchronisation.
//
// The client imports a real DRM timeline syncobj, names an acquire point that
// is already signalled and a release point that is not, and commits. The server
// must wait on the acquire point (returning immediately) and then, when the
// buffer is superseded by the next commit, signal the release point — which the
// client proves by waiting on it with a zero timeout.
//
// Skips (exit 77) when there is no render node with timeline syncobj support.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <xf86drm.h>

#include "linux-drm-syncobj-v1-client-protocol.h"

import luminaria.gpu;
import std;

namespace {

constexpr uint64_t kAcquirePoint = 1;
constexpr uint64_t kReleasePoint = 2;

int open_render_node() {
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

struct ClientState {
    wl_compositor* compositor = nullptr;
    wp_linux_drm_syncobj_manager_v1* syncobj_manager = nullptr;
    bool release_signalled = false;
};

ClientState g_client;

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wp_linux_drm_syncobj_manager_v1") == 0) {
        st->syncobj_manager = static_cast<wp_linux_drm_syncobj_manager_v1*>(
            wl_registry_bind(registry, name, &wp_linux_drm_syncobj_manager_v1_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

void run_client(int fd, int drm_fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState& st = g_client;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display);

    uint32_t handle = 0;
    if (st.compositor != nullptr && st.syncobj_manager != nullptr &&
        drmSyncobjCreate(drm_fd, 0, &handle) == 0) {
        int syncobj_fd = -1;
        if (drmSyncobjHandleToFD(drm_fd, handle, &syncobj_fd) == 0) {
            // The GPU work is "already done": signal the acquire point up front
            // so the server's wait returns immediately.
            uint64_t point = kAcquirePoint;
            drmSyncobjTimelineSignal(drm_fd, &handle, &point, 1);

            wp_linux_drm_syncobj_timeline_v1* timeline =
                wp_linux_drm_syncobj_manager_v1_import_timeline(st.syncobj_manager, syncobj_fd);
            close(syncobj_fd);

            wl_surface* surface = wl_compositor_create_surface(st.compositor);
            wp_linux_drm_syncobj_surface_v1* sync =
                wp_linux_drm_syncobj_manager_v1_get_surface(st.syncobj_manager, surface);

            wp_linux_drm_syncobj_surface_v1_set_acquire_point(sync, timeline, 0, kAcquirePoint);
            wp_linux_drm_syncobj_surface_v1_set_release_point(sync, timeline, 0, kReleasePoint);
            wl_surface_commit(surface);
            wl_display_roundtrip(display);

            // The next commit supersedes that buffer: the release point fires.
            wl_surface_commit(surface);
            wl_display_roundtrip(display);

            uint64_t release = kReleasePoint;
            const int ret = drmSyncobjTimelineWait(drm_fd, &handle, &release, 1, 0,
                                                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, nullptr);
            st.release_signalled = ret == 0;
        }
        drmSyncobjDestroy(drm_fd, handle);
    }
    wl_display_disconnect(display);
}

struct DestroyCtx {
    wl_listener listener;
    luminaria::Display* display;
};
void on_client_destroy(wl_listener* l, void*) {
    auto* ctx = reinterpret_cast<DestroyCtx*>(reinterpret_cast<char*>(l) -
                                              offsetof(DestroyCtx, listener));
    ctx->display->terminate();
}

} // namespace

int main() {
    const int drm_fd = open_render_node();
    if (drm_fd < 0) {
        return 77; // no GPU with timeline syncobj support here
    }

    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto syncobj = luminaria::DrmSyncobjManager::create(*display);
    if (!syncobj) {
        close(drm_fd);
        return 77;
    }
    syncobj->set_acquire_timeout_ms(500);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    int commits = 0;
    int acquire_fences = 0;
    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        conns.push_back(e.surface.commit.connect([&](luminaria::SurfaceCommit& ce) {
            ++commits;
            // The acquire point is exported as a sync_file rather than waited
            // for on the CPU — that fd is what the renderer hands the GPU.
            if (ce.surface.acquire_fence_fd() >= 0) {
                ++acquire_fences;
            }
        }));
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_client, fds[1], drm_fd);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(5000);

    display->run();
    client_thread.join();
    close(drm_fd);

    assert(commits == 2);
    assert(g_client.release_signalled);
    // The first commit named an acquire point, so the surface must have come
    // out of it holding a fence. (The second names none.)
    assert(acquire_fences == 1);
    return 0;
}
