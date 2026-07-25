// luminaria/session.hpp — the seat session: who is allowed to touch the GPU and
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
#pragma once

#include <memory>

#include "luminaria/core/event_loop.hpp"
#include "luminaria/core/expected.hpp"
#include "luminaria/core/signal.hpp"

namespace luminaria {

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
