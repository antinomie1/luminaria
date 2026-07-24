#include "luminaria/core/event_loop.hpp"

#include <utility>

#include <wayland-server-core.h>

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
