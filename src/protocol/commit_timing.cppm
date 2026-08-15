// luminaria/commit_timing.cppm — wp_commit_timing_manager_v1: "show this frame
// at this instant, not before".
//
// The other half of pacing. FIFO says "after the last one"; commit timing names
// the moment. A video player that knows a frame belongs to presentation time T
// can hand it over early and stop guessing how long the compositor will take,
// which is what lets it sleep instead of spinning near the deadline.
//
// `set_timestamp` applies to the next wl_surface.commit and parks it on the
// Surface's commit gate (the same one wp_fifo_v1 uses). Something has to wake
// the loop when the deadline arrives, and that is this global's one moving
// part: an EventLoop timer armed to the earliest deadline outstanding. Nothing
// is asked of the compositor beyond creating the global.

module;

#include <cstddef>
#include <cstdint>
#include <memory>

#include <algorithm>
#include <ctime>
#include <limits>
#include <typeinfo>
#include <utility>
#include <vector>
#include <wayland-server-core.h>
#include "commit-timing-v1-protocol.h"

export module luminaria:commit_timing;

import :compositor;
import :display;
import :event_loop;
import :expected;
import :signal;

export namespace luminaria {

class Display;

/// The wp_commit_timing_manager_v1 global (version 1). Move-only;
/// pointer-stable state so the libwayland global can hold a pointer to it.
class CommitTimingManager {
public:
    [[nodiscard]] static Result<CommitTimingManager> create(Display& display);

    ~CommitTimingManager();
    CommitTimingManager(CommitTimingManager&&) noexcept;
    CommitTimingManager& operator=(CommitTimingManager&&) noexcept;
    CommitTimingManager(const CommitTimingManager&) = delete;
    CommitTimingManager& operator=(const CommitTimingManager&) = delete;

    /// Apply every commit whose timestamp has come. Driven by this global's own
    /// timer; exposed because a compositor that is already awake for a frame
    /// may as well flush the ones that came due while it rendered.
    void release_due();

    /// How many wp_commit_timer_v1 objects are alive. Mostly for tests.
    [[nodiscard]] std::size_t timer_count() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit CommitTimingManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Implements wp_commit_timing_manager_v1 (version 1).

namespace luminaria {

// Not in an anonymous namespace: CommitTimingManager::Impl holds these.
struct CommitTimer;

struct CommitTimingManager::Impl {
    wl_global* global = nullptr;
    std::vector<CommitTimer*> timers;
    EventSource wakeup;

    /// Apply what is due and re-arm for the earliest deadline still ahead.
    void tick();

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

using CtMgr = CommitTimingManager::Impl;

// One wp_commit_timer_v1. `deadline` is the stamp staged for the next commit
// (0 = none) — the manager keeps it so it can arm a timer without waiting for
// the commit that will eventually park on it.
struct CommitTimer {
    CtMgr* mgr = nullptr;
    SurfaceId surface;
    std::uint64_t deadline = 0;
};

namespace {

std::uint64_t monotonic_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

void timer_set_timestamp(wl_client*, wl_resource* resource, uint32_t tv_sec_hi,
                         uint32_t tv_sec_lo, uint32_t tv_nsec) {
    auto* timer = static_cast<CommitTimer*>(wl_resource_get_user_data(resource));
    Surface* surface = surface_from_id(timer->surface);
    if (surface == nullptr) {
        wl_resource_post_error(resource, WP_COMMIT_TIMER_V1_ERROR_SURFACE_DESTROYED,
                               "the wl_surface this wp_commit_timer_v1 was made for is gone");
        return;
    }
    if (tv_nsec >= 1'000'000'000U) {
        wl_resource_post_error(resource, WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP,
                               "tv_nsec must be below one second");
        return;
    }
    if (surface->pending_commit_time() != 0) {
        // One timestamp per commit: the protocol says a second is an error
        // rather than a replacement. The Surface is the one that knows, because
        // its pending state is what a commit resets.
        wl_resource_post_error(resource, WP_COMMIT_TIMER_V1_ERROR_TIMESTAMP_EXISTS,
                               "a timestamp is already staged for the next commit");
        return;
    }
    const std::uint64_t seconds =
        (static_cast<std::uint64_t>(tv_sec_hi) << 32) | static_cast<std::uint64_t>(tv_sec_lo);
    timer->deadline = seconds * 1'000'000'000ULL + tv_nsec;
    surface->set_pending_commit_time(timer->deadline);
    timer->mgr->tick();
}

void timer_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void timer_resource_destroy(wl_resource* resource) {
    auto* timer = static_cast<CommitTimer*>(wl_resource_get_user_data(resource));
    // Whatever this timer parked must not stay parked: apply it now rather than
    // leaving the client's last frame invisible forever.
    if (Surface* surface = surface_from_id(timer->surface); surface != nullptr) {
        surface->release_deferred_commit(std::numeric_limits<std::uint64_t>::max());
    }
    std::erase(timer->mgr->timers, timer);
    delete timer;
}

constexpr struct wp_commit_timer_v1_interface timer_impl = {
    .set_timestamp = timer_set_timestamp,
    .destroy = timer_destroy_request,
};

void manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void manager_get_timer(wl_client* client, wl_resource* manager, uint32_t id,
                       wl_resource* surface_resource) {
    auto* mgr = static_cast<CtMgr*>(wl_resource_get_user_data(manager));
    Surface* surface = surface_from_resource(surface_resource);
    if (surface != nullptr) {
        const SurfaceId sid = surface->id();
        const bool duplicate = std::ranges::any_of(
            mgr->timers, [sid](const CommitTimer* t) { return t->surface == sid; });
        if (duplicate) {
            wl_resource_post_error(manager, WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS,
                                   "this wl_surface already has a wp_commit_timer_v1");
            return;
        }
    }
    wl_resource* resource = wl_resource_create(client, &wp_commit_timer_v1_interface,
                                               wl_resource_get_version(manager),
                                               static_cast<int>(id));
    if (resource == nullptr) {
        wl_resource_post_no_memory(manager);
        return;
    }
    auto* timer = new CommitTimer{mgr, surface != nullptr ? surface->id() : SurfaceId{}, 0};
    mgr->timers.push_back(timer);
    wl_resource_set_implementation(resource, &timer_impl, timer, timer_resource_destroy);
}

constexpr struct wp_commit_timing_manager_v1_interface manager_impl = {
    .destroy = manager_destroy_request,
    .get_timer = manager_get_timer,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wp_commit_timing_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

} // namespace

void CommitTimingManager::Impl::tick() {
    const std::uint64_t now = monotonic_ns();
    std::uint64_t next = 0;
    // Copy: applying a commit emits its `commit` signal, and a handler may
    // create or destroy timers.
    const std::vector<CommitTimer*> snapshot = timers;
    for (CommitTimer* timer : snapshot) {
        if (timer->deadline == 0) {
            continue;
        }
        Surface* surface = surface_from_id(timer->surface);
        if (surface == nullptr) {
            timer->deadline = 0;
            continue;
        }
        if (timer->deadline <= now) {
            // Due. The stamp is per-commit, so it is spent either way: the
            // surface either had a commit parked on it or never made one.
            timer->deadline = 0;
            surface->release_deferred_commit(now);
            continue;
        }
        if (next == 0 || timer->deadline < next) {
            next = timer->deadline;
        }
    }
    if (next == 0) {
        wakeup.disarm();
        return;
    }
    // Round up: waking a millisecond early only means one more tick that finds
    // nothing due, while waking early-by-truncation would spin.
    const std::uint64_t ms = (next - now + 999'999ULL) / 1'000'000ULL;
    wakeup.arm(static_cast<unsigned>(std::min<std::uint64_t>(ms, 60'000)));
}

CommitTimingManager::CommitTimingManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
CommitTimingManager::~CommitTimingManager() = default;
CommitTimingManager::CommitTimingManager(CommitTimingManager&&) noexcept = default;
CommitTimingManager& CommitTimingManager::operator=(CommitTimingManager&&) noexcept = default;

Result<CommitTimingManager> CommitTimingManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->global = wl_global_create(display.c_ptr(), &wp_commit_timing_manager_v1_interface, 1,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wp_commit_timing_manager_v1) failed");
    }
    Impl* raw = impl.get();
    impl->wakeup = display.event_loop().add_timer([raw] { raw->tick(); });
    if (!impl->wakeup.valid()) {
        return fail("wl_event_loop_add_timer for wp_commit_timing_manager_v1 failed");
    }
    return CommitTimingManager{std::move(impl)};
}

void CommitTimingManager::release_due() {
    impl_->tick();
}

std::size_t CommitTimingManager::timer_count() const noexcept {
    return impl_->timers.size();
}

} // namespace luminaria
