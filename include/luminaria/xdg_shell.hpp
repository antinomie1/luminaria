// luminaria/xdg_shell.hpp — xdg-shell: application windows (toplevels) and popups.
//
// A toplevel is a normal window. Lifecycle: client creates an xdg_surface from a
// wl_surface, gives it the toplevel role, does the initial commit (no buffer),
// the server sends a configure, the client acks + attaches a buffer + commits,
// and the toplevel MAPS (becomes visible). Popups are deferred (YAGNI until menus).
#pragma once

#include <memory>

#include "luminaria/core/expected.hpp"
#include "luminaria/core/signal.hpp"

namespace luminaria {

class Display;
class Surface;
class Toplevel;

struct NewToplevel {
    Toplevel& toplevel;
};
struct ToplevelMap {
    Toplevel& toplevel;
};
struct ToplevelDestroy {
    Toplevel& toplevel;
};

/// A mapped/mapping application window. Owned by its xdg_surface; address stable.
class Toplevel {
public:
    virtual ~Toplevel() = default;
    Toplevel(const Toplevel&) = delete;
    Toplevel& operator=(const Toplevel&) = delete;

    Signal<ToplevelMap> map;
    Signal<ToplevelDestroy> destroy;

    [[nodiscard]] virtual Surface& surface() noexcept = 0;
    [[nodiscard]] virtual bool mapped() const noexcept = 0;

protected:
    Toplevel() = default;
};

/// The xdg_wm_base global. Move-only; state is pointer-stable (pimpl).
class XdgShell {
public:
    [[nodiscard]] static Result<XdgShell> create(Display& display);

    ~XdgShell();
    XdgShell(XdgShell&&) noexcept;
    XdgShell& operator=(XdgShell&&) noexcept;
    XdgShell(const XdgShell&) = delete;
    XdgShell& operator=(const XdgShell&) = delete;

    /// Fires when a client creates a toplevel window.
    [[nodiscard]] Signal<NewToplevel>& new_toplevel() noexcept;

    struct Impl; // named by the protocol glue

private:
    std::unique_ptr<Impl> impl_;
    explicit XdgShell(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
