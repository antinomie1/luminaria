// luminaria/xwayland.hpp — run X11 apps by launching Xwayland and managing its
// windows. Spawns the Xwayland server (rootless), connects a minimal X window
// manager over xcb, and redirects the root so X windows can be mapped.
//
// TODO: minimal XWM — map/configure requests handled; full ICCCM/EWMH,
// override-redirect, and wl_surface association land as real X clients need them.
#pragma once

#include <memory>
#include <string>

#include "luminaria/core/expected.hpp"
#include "luminaria/core/signal.hpp"

namespace luminaria {

class Display;
class Compositor;

struct XwaylandReady {
    std::string display_name; // e.g. ":1"
};

class Xwayland {
public:
    /// Launch Xwayland connected to this compositor. The parent compositor must
    /// already have a socket (WAYLAND_DISPLAY) that Xwayland can connect to.
    [[nodiscard]] static Result<Xwayland> create(Display& display, Compositor& compositor);

    ~Xwayland();
    Xwayland(Xwayland&&) noexcept;
    Xwayland& operator=(Xwayland&&) noexcept;
    Xwayland(const Xwayland&) = delete;
    Xwayland& operator=(const Xwayland&) = delete;

    /// Fires once the X server is up and the window manager has attached.
    [[nodiscard]] Signal<XwaylandReady>& ready() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit Xwayland(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
