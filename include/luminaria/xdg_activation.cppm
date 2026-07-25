// luminaria/xdg_activation.cppm — xdg_activation_v1: "raise that window".
//
// The protocol behind clicking a link in a chat window and having the browser
// come to the front. It is a two-step handshake, and the second step happens in
// a different client:
//
//   1. The client that has the user's attention asks for a TOKEN, naming the
//      seat and the input serial that justifies it (`new_token`).
//   2. It passes the token to the other client out of band (an env var, a D-Bus
//      call), which hands it back with `activate` on the surface it wants
//      raised (`request_activate`).
//
// The token is what keeps this from being a focus-stealing hole: a compositor
// that cares checks the serial in `new_token` against real recent input, and
// refuses the ones it does not recognise. Nothing here raises a window on its
// own — `request_activate` is a request, and giving focus is the compositor's
// call. Tokens are single-use.

module;

#include "luminaria/detail/wayland_fwd.h"

#include <cstdint>
#include <memory>
#include <string>

export module luminaria:xdg_activation;

import :core.expected;
import :core.signal;

export namespace luminaria {

class Display;
class Surface;

/// A client is asking for an activation token. Set `granted` to false to hand
/// out a token that will be marked invalid when it comes back — refusing here
/// rather than at activation time is what stops a background application from
/// stealing focus.
struct ActivationTokenRequest {
    wl_client* client;
    /// The wl_seat the client named, or null if it named none.
    wl_resource* seat;
    /// The input event serial the client offered as justification (0 = none).
    /// Check it against the last serial you actually sent that client.
    std::uint32_t serial;
    /// The surface the requesting client says it is acting on behalf of.
    Surface* surface;
    /// The application the token is meant for ("org.example.Browser"), empty if
    /// the client did not say.
    std::string app_id;
    bool granted; // in: true; out: the decision
};

/// A client handed back a token and wants `surface` raised and focused.
struct ActivationRequest {
    Surface& surface;
    /// False when the token is unknown, already used, or was refused at
    /// `new_token` time. Honouring one of those is exactly the focus steal the
    /// protocol exists to prevent.
    bool token_valid;
    /// State recorded when the token was created (empty / null if it was not).
    std::string app_id;
    Surface* requesting_surface;
    wl_resource* seat;
    std::uint32_t serial;
};

/// The xdg_activation_v1 global (version 1). Move-only; pointer-stable state.
class XdgActivation {
public:
    [[nodiscard]] static Result<XdgActivation> create(Display& display);

    ~XdgActivation();
    XdgActivation(XdgActivation&&) noexcept;
    XdgActivation& operator=(XdgActivation&&) noexcept;
    XdgActivation(const XdgActivation&) = delete;
    XdgActivation& operator=(const XdgActivation&) = delete;

    /// Fires when a client commits an xdg_activation_token_v1.
    [[nodiscard]] Signal<ActivationTokenRequest>& new_token() noexcept;
    /// Fires when a client calls xdg_activation_v1.activate.
    [[nodiscard]] Signal<ActivationRequest>& request_activate() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit XdgActivation(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
