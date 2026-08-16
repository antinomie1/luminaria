// luminaria/session.cppm — the seat session: who is allowed to touch the GPU and
// the input devices right now.
//
// A bare-metal compositor cannot just open /dev/dri/card0 and keep it. The
// moment the user presses Ctrl+Alt+F2 the kernel hands the VT to someone else,
// and a compositor that has not given up DRM master takes the display with it.
// libseat (talking to logind, or seatd) is the arbiter: it opens devices on our
// behalf and tells us when the session goes away and comes back.
//
// Without a Session the backends fall back to opening devices directly, which
// works from a logged-in VT thanks to logind's ACLs — but VT switching is then
// unsafe, and that is the whole difference.
//
// Usage: create the Session first, hand it to `DrmBackend::create` and
// `LibinputBackend::create`, and watch `activity` to stop drawing while the
// session is inactive.

module;


// libseat.h has no extern "C" guard of its own, so C++ would mangle every
// symbol in it. This is the same treatment the other C headers get implicitly.
// It has to sit here in the global module fragment: an #include below the module
// declaration would attach libseat's declarations to luminaria:session.
extern "C" {
#include <libseat.h>
}

export module luminaria.gpu:session;

import std;

import luminaria;

export namespace luminaria {

/// "The session became (in)active." On `false` every device fd we hold has been
/// revoked by the kernel: stop rendering, stop committing, and expect ioctls to
/// fail until it comes back. On `true` the devices work again — re-apply the
/// modeset, because someone else has been using the screen.
struct SessionActive {
    bool active;
};

class Session {
public:
    /// Join the seat this process is logged into. Fails when there is no seat to
    /// join (no logind session, no seatd) — that is not an error for a nested or
    /// headless compositor, only for a bare-metal one.
    [[nodiscard]] static Result<Session> create(EventLoop loop);

    ~Session();
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Open a device through the seat. The returned fd is ours to use but not to
    /// close — pass the id back to `close_device`. The seat may revoke access at
    /// any time (see `activity`) without the fd becoming invalid.
    [[nodiscard]] Result<int> open_device(const char* path, int& device_id);
    void close_device(int device_id);

    /// False between a VT switch away and the switch back.
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] Signal<SessionActive>& activity() noexcept;

    /// Ask to switch to another VT (1-based), as a compositor key binding would.
    Status switch_to(int vt);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit Session(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct Session::Impl {
    EventLoop loop;
    libseat* seat = nullptr;
    EventSource seat_source;
    bool active = false;
    Signal<SessionActive> activity;

    ~Impl() {
        seat_source = EventSource{};
        if (seat != nullptr) {
            libseat_close_seat(seat);
        }
    }

    void set_active(bool now) {
        if (active == now) {
            return;
        }
        active = now;
        SessionActive event{now};
        activity.emit(event);
    }
};

namespace {

void on_enable(libseat*, void* data) {
    static_cast<Session::Impl*>(data)->set_active(true);
}

void on_disable(libseat* seat, void* data) {
    auto* impl = static_cast<Session::Impl*>(data);
    // Order matters: tell our listeners first so they drop DRM master and stop
    // committing, THEN acknowledge. libseat hands the VT over the moment we
    // acknowledge, and anything still holding the display at that point wins a
    // black screen for the next session.
    impl->set_active(false);
    libseat_disable_seat(seat);
}

constexpr libseat_seat_listener kListener{on_enable, on_disable};

} // namespace

Session::Session(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Session::~Session() = default;
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;

Result<Session> Session::create(EventLoop loop) {
    auto impl = std::make_unique<Impl>();
    impl->loop = loop;
    impl->seat = libseat_open_seat(const_cast<libseat_seat_listener*>(&kListener), impl.get());
    if (impl->seat == nullptr) {
        return fail("session: no seat (need a logind session or seatd)");
    }
    // The seat is not usable until it has sent us the first enable_seat. It is
    // already queued, so one dispatch is enough; loop in case it isn't.
    for (int i = 0; i < 16 && !impl->active; ++i) {
        if (libseat_dispatch(impl->seat, 1000) < 0) {
            return fail("session: libseat_dispatch failed while activating");
        }
    }
    if (!impl->active) {
        return fail("session: the seat never activated");
    }

    Impl* raw = impl.get();
    impl->seat_source =
        loop.add_fd(libseat_get_fd(impl->seat), [raw] { libseat_dispatch(raw->seat, 0); });
    return Session{std::move(impl)};
}

Result<int> Session::open_device(const char* path, int& device_id) {
    int fd = -1;
    const int id = libseat_open_device(impl_->seat, path, &fd);
    if (id < 0 || fd < 0) {
        return fail(std::string{"session: cannot open "} + path);
    }
    device_id = id;
    return fd;
}

void Session::close_device(int device_id) {
    if (device_id >= 0) {
        libseat_close_device(impl_->seat, device_id);
    }
}

bool Session::active() const noexcept { return impl_->active; }

Signal<SessionActive>& Session::activity() noexcept { return impl_->activity; }

Status Session::switch_to(int vt) {
    if (libseat_switch_session(impl_->seat, vt) != 0) {
        return fail("session: switch_session failed");
    }
    return ok();
}

} // namespace luminaria
