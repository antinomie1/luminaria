// luminaria/fifo.cppm — wp_fifo_manager_v1: "pace me to the display".
//
// A client that animates has no way to say "this frame replaces the last one
// ONLY after the last one has been shown". Without that it either renders as
// fast as the CPU allows and throws most of it away, or it waits for a frame
// callback and pays a full round trip of latency before it may even start.
// FIFO gives it the third option, which is what a swapchain in FIFO present
// mode has always meant: queue the frame, and let the compositor apply it when
// the previous content has been on screen.
//
// Two requests, both applying to the next wl_surface.commit:
//
//   * `set_barrier` — this content owes the display one presentation.
//   * `wait_barrier` — do not apply this commit until that debt is paid.
//
// The gate itself lives on the Surface (`Surface::fifo_barrier()`,
// `release_deferred_commit()`), because a parked commit must be invisible to
// everything downstream — no commit signal, no damage, no frame callbacks. The
// barrier is cleared by `Surface::send_frame_done()`, so a compositor that
// already answers frame callbacks at presentation needs no new wiring at all.
//
// The one thing it MUST do is call `clear_fifo_barrier()` for a surface it is
// not presenting — a minimised or occluded client is otherwise blocked for
// good. `FifoManager::unblock_hidden()` is that, for every surface at once.

module;

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <algorithm>
#include <typeinfo>
#include <utility>
#include <vector>
#include <wayland-server-core.h>
#include "fifo-v1-protocol.h"

export module luminaria:fifo;

import :compositor;
import :display;
import :expected;
import :signal;

export namespace luminaria {

class Display;

/// The wp_fifo_manager_v1 global (version 1). Move-only; pointer-stable state
/// so the libwayland global can hold a pointer to it.
class FifoManager {
public:
    [[nodiscard]] static Result<FifoManager> create(Display& display);

    ~FifoManager();
    FifoManager(FifoManager&&) noexcept;
    FifoManager& operator=(FifoManager&&) noexcept;
    FifoManager(const FifoManager&) = delete;
    FifoManager& operator=(const FifoManager&) = delete;

    /// Drop the barrier of every surface with a FIFO object that `keep` does not
    /// vouch for, letting whatever they parked through. Call it once per frame
    /// with a predicate that answers "did I just present this surface?" — the
    /// protocol requires a surface that will never be shown to stop blocking,
    /// and this library cannot know which those are.
    void unblock_hidden(const std::function<bool(Surface&)>& keep);

    /// How many wp_fifo_v1 objects are alive. Mostly for tests.
    [[nodiscard]] std::size_t fifo_count() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit FifoManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Implements wp_fifo_manager_v1 (version 1). All this file owns is the objects
// and the duplicate check; the semantics are the Surface's commit gate.

namespace luminaria {

// Not in an anonymous namespace: FifoManager::Impl holds these.
struct FifoObject;

struct FifoManager::Impl {
    wl_global* global = nullptr;
    std::vector<FifoObject*> objects;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

using FifoMgr = FifoManager::Impl;

// One wp_fifo_v1. The surface may die first — the protocol has an error for
// using the object afterwards, so the id is kept to answer that rather than
// silently doing nothing.
struct FifoObject {
    FifoMgr* mgr = nullptr;
    SurfaceId surface;
};

namespace {

/// The surface this object was made for, or null once it has gone. Generational,
/// so a recycled slot can never resolve to a different client's surface.
Surface* fifo_surface(FifoObject* object) {
    return surface_from_id(object->surface);
}

void fifo_set_barrier(wl_client*, wl_resource* resource) {
    auto* object = static_cast<FifoObject*>(wl_resource_get_user_data(resource));
    Surface* surface = fifo_surface(object);
    if (surface == nullptr) {
        wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
                               "the wl_surface this wp_fifo_v1 was made for is gone");
        return;
    }
    surface->set_pending_fifo_barrier();
}

void fifo_wait_barrier(wl_client*, wl_resource* resource) {
    auto* object = static_cast<FifoObject*>(wl_resource_get_user_data(resource));
    Surface* surface = fifo_surface(object);
    if (surface == nullptr) {
        wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
                               "the wl_surface this wp_fifo_v1 was made for is gone");
        return;
    }
    surface->set_pending_fifo_wait();
}

void fifo_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void fifo_resource_destroy(wl_resource* resource) {
    auto* object = static_cast<FifoObject*>(wl_resource_get_user_data(resource));
    // Destroying the object must not leave a commit parked forever.
    if (Surface* surface = fifo_surface(object); surface != nullptr) {
        surface->clear_fifo_barrier();
    }
    std::erase(object->mgr->objects, object);
    delete object;
}

constexpr struct wp_fifo_v1_interface fifo_impl = {
    .set_barrier = fifo_set_barrier,
    .wait_barrier = fifo_wait_barrier,
    .destroy = fifo_destroy_request,
};

void manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void manager_get_fifo(wl_client* client, wl_resource* manager, uint32_t id,
                      wl_resource* surface_resource) {
    auto* mgr = static_cast<FifoMgr*>(wl_resource_get_user_data(manager));
    Surface* surface = surface_from_resource(surface_resource);
    if (surface != nullptr) {
        const SurfaceId sid = surface->id();
        const bool duplicate = std::ranges::any_of(
            mgr->objects, [sid](const FifoObject* o) { return o->surface == sid; });
        if (duplicate) {
            wl_resource_post_error(manager, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
                                   "this wl_surface already has a wp_fifo_v1");
            return;
        }
    }
    wl_resource* resource = wl_resource_create(client, &wp_fifo_v1_interface,
                                               wl_resource_get_version(manager),
                                               static_cast<int>(id));
    if (resource == nullptr) {
        wl_resource_post_no_memory(manager);
        return;
    }
    auto* object = new FifoObject{mgr, surface != nullptr ? surface->id() : SurfaceId{}};
    mgr->objects.push_back(object);
    wl_resource_set_implementation(resource, &fifo_impl, object, fifo_resource_destroy);
}

constexpr struct wp_fifo_manager_v1_interface manager_impl = {
    .destroy = manager_destroy_request,
    .get_fifo = manager_get_fifo,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wp_fifo_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

} // namespace

FifoManager::FifoManager(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
FifoManager::~FifoManager() = default;
FifoManager::FifoManager(FifoManager&&) noexcept = default;
FifoManager& FifoManager::operator=(FifoManager&&) noexcept = default;

Result<FifoManager> FifoManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->global = wl_global_create(display.c_ptr(), &wp_fifo_manager_v1_interface, 1, impl.get(),
                                    manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wp_fifo_manager_v1) failed");
    }
    return FifoManager{std::move(impl)};
}

void FifoManager::unblock_hidden(const std::function<bool(Surface&)>& keep) {
    // Copy: clearing a barrier applies a parked commit, whose `commit` signal
    // can create or destroy fifo objects under us.
    const std::vector<FifoObject*> objects = impl_->objects;
    for (FifoObject* object : objects) {
        Surface* surface = surface_from_id(object->surface);
        if (surface != nullptr && surface->fifo_barrier() && !keep(*surface)) {
            surface->clear_fifo_barrier();
        }
    }
}

std::size_t FifoManager::fifo_count() const noexcept {
    return impl_->objects.size();
}

} // namespace luminaria
