// luminaria/core/event_loop.hpp — thin, non-owning view over a wl_event_loop.
//
// The loop is owned by the Display; EventLoop is a lightweight handle onto it.
// Public header stays free of <wayland-server-core.h> (forward decls only).
#pragma once

#include <functional>

struct wl_event_loop;   // opaque, from libwayland-server
struct wl_event_source; // opaque, from libwayland-server

namespace luminaria {

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
