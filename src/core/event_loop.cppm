// luminaria/core/event_loop.cppm — thin, non-owning view over a wl_event_loop.
//
// The loop is owned by the Display; EventLoop is a lightweight handle onto it.
// Importing luminaria pulls in no <wayland-server-core.h>: the loop and source
// types are forward-declared in the global module fragment.

module;

#include "detail/wayland_fwd.h"
#include <typeinfo>

#include <wayland-server-core.h>

export module luminaria:event_loop;

import std;
export namespace luminaria {

/// RAII owner of a persistent loop source (e.g. a timer). Removes it on destroy.
/// Move-only. The callback is owned here and freed with the source.
class EventSource {
    wl_event_source* source_ = nullptr;
    std::function<void()>* callback_ = nullptr; // owned

    friend class EventLoop;
    EventSource(wl_event_source* source, std::function<void()>* callback) noexcept
        : source_(source), callback_(callback) {}

public:
    EventSource() = default;
    EventSource(EventSource&& o) noexcept
        : source_(o.source_), callback_(o.callback_) {
        o.source_ = nullptr;
        o.callback_ = nullptr;
    }
    EventSource& operator=(EventSource&& o) noexcept;
    EventSource(const EventSource&) = delete;
    EventSource& operator=(const EventSource&) = delete;
    ~EventSource();

    [[nodiscard]] bool valid() const noexcept { return source_ != nullptr; }

    /// Fire the callback once after `ms` milliseconds (one-shot; re-arm inside
    /// the callback for a repeating source).
    void arm(unsigned ms);

    /// Disarm a pending timer without destroying the source.
    void disarm();
};

class EventLoop {
    wl_event_loop* loop_ = nullptr; // non-owning

public:
    EventLoop() = default;
    explicit EventLoop(wl_event_loop* loop) noexcept : loop_(loop) {}

    [[nodiscard]] bool valid() const noexcept { return loop_ != nullptr; }
    [[nodiscard]] wl_event_loop* c_ptr() const noexcept { return loop_; }

    /// Run `fn` once on the next idle turn of the loop, then drop it.
    /// (Idle sources are one-shot in libwayland; ownership of `fn` is handled here.)
    void once(std::function<void()> fn);

    /// Create a disarmed timer whose callback is `fn`. Call `arm(ms)` to start it.
    [[nodiscard]] EventSource add_timer(std::function<void()> fn);

    /// Watch `fd` for readability; run `fn` when readable. RAII removes the watch.
    [[nodiscard]] EventSource add_fd(int fd, std::function<void()> fn);
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

EventSource& EventSource::operator=(EventSource&& o) noexcept {
    if (this != &o) {
        if (source_ != nullptr) {
            wl_event_source_remove(source_);
        }
        delete callback_;
        source_ = o.source_;
        callback_ = o.callback_;
        o.source_ = nullptr;
        o.callback_ = nullptr;
    }
    return *this;
}

EventSource::~EventSource() {
    if (source_ != nullptr) {
        wl_event_source_remove(source_); // frees the wl side
    }
    delete callback_;
}

void EventSource::arm(unsigned ms) {
    if (source_ != nullptr) {
        wl_event_source_timer_update(source_, static_cast<int>(ms));
    }
}

void EventSource::disarm() {
    if (source_ != nullptr) {
        wl_event_source_timer_update(source_, 0);
    }
}

void EventLoop::once(std::function<void()> fn) {
    if (loop_ == nullptr) {
        return;
    }
    // libwayland idle sources fire once and are then removed automatically, so we
    // only need to own the callback until it runs. Heap it, free it in the trampoline.
    auto* held = new std::function<void()>(std::move(fn));
    wl_event_loop_add_idle(
        loop_,
        [](void* data) {
            auto* f = static_cast<std::function<void()>*>(data);
            (*f)();
            delete f;
        },
        held);
}

EventSource EventLoop::add_timer(std::function<void()> fn) {
    if (loop_ == nullptr) {
        return EventSource{};
    }
    auto* held = new std::function<void()>(std::move(fn));
    wl_event_source* src = wl_event_loop_add_timer(
        loop_,
        [](void* data) -> int {
            (*static_cast<std::function<void()>*>(data))();
            return 0;
        },
        held);
    if (src == nullptr) {
        delete held;
        return EventSource{};
    }
    return EventSource{src, held};
}

EventSource EventLoop::add_fd(int fd, std::function<void()> fn) {
    if (loop_ == nullptr) {
        return EventSource{};
    }
    auto* held = new std::function<void()>(std::move(fn));
    wl_event_source* src = wl_event_loop_add_fd(
        loop_, fd, WL_EVENT_READABLE,
        [](int, uint32_t, void* data) -> int {
            (*static_cast<std::function<void()>*>(data))();
            return 0;
        },
        held);
    if (src == nullptr) {
        delete held;
        return EventSource{};
    }
    return EventSource{src, held};
}

} // namespace luminaria
