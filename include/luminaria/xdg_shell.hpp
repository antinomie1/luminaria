// luminaria/xdg_shell.hpp — xdg-shell: application windows (toplevels) and popups.
//
// A toplevel is a normal window. Lifecycle: client creates an xdg_surface from a
// wl_surface, gives it the toplevel role, does the initial commit (no buffer),
// the server sends a configure, the client acks + attaches a buffer + commits,
// and the toplevel MAPS (becomes visible).
//
// A popup is a menu / tooltip / combo drop-down. It is positioned by an
// xdg_positioner relative to its parent's window geometry; luminaria evaluates
// the positioner and reports the resulting offset as Popup::x()/y().
//
// The compositor drives window state: it decides whether a maximize request is
// granted and calls set_maximized()/configure() to tell the client.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "luminaria/core/expected.hpp"
#include "luminaria/core/signal.hpp"
#include "luminaria/util/box.hpp"

struct wl_resource; // opaque, from libwayland-server

namespace luminaria {

class Display;
class Surface;
class Toplevel;
class Popup;

struct NewToplevel {
    Toplevel& toplevel;
};
struct ToplevelMap {
    Toplevel& toplevel;
};
struct ToplevelUnmap {
    Toplevel& toplevel;
};
struct ToplevelDestroy {
    Toplevel& toplevel;
};
/// The client asked for a state change. The compositor decides and answers with
/// set_maximized()/set_fullscreen()/configure(); ignoring the request is legal.
struct ToplevelRequestMaximize {
    Toplevel& toplevel;
    bool maximized;
};
struct ToplevelRequestFullscreen {
    Toplevel& toplevel;
    bool fullscreen;
};
struct ToplevelRequestMinimize {
    Toplevel& toplevel;
};
/// Interactive move/resize, started from a click. `serial` is the input serial
/// the client passed; `edges` is an xdg_toplevel.resize_edge bitmask.
struct ToplevelRequestMove {
    Toplevel& toplevel;
    std::uint32_t serial;
};
struct ToplevelRequestResize {
    Toplevel& toplevel;
    std::uint32_t serial;
    std::uint32_t edges;
};
/// title / app_id changed.
struct ToplevelIdentityChange {
    Toplevel& toplevel;
};
/// The client declared this window transient for another (xdg_toplevel.set_parent).
/// `parent` is null when the relationship was cleared or the parent has gone.
/// A compositor should keep the child stacked above its parent and, for dialogs,
/// centre it there.
struct ToplevelParentChange {
    Toplevel& toplevel;
    Toplevel* parent;
};
/// Right-click on the client's own decorations: it wants US to show the window
/// menu (close / maximize / move to workspace) at (x,y) in its surface
/// coordinates. Ignoring it is legal — there just won't be a menu.
struct ToplevelRequestWindowMenu {
    Toplevel& toplevel;
    std::uint32_t serial;
    int x, y;
};

/// A rectangle in surface-local coordinates (xdg window geometry, popup extent).
struct XdgGeometry {
    int x = 0, y = 0, width = 0, height = 0;
};

/// A mapped/mapping application window. Owned by its xdg_toplevel resource;
/// address stable for its lifetime.
class Toplevel {
public:
    virtual ~Toplevel() = default;
    Toplevel(const Toplevel&) = delete;
    Toplevel& operator=(const Toplevel&) = delete;

    Signal<ToplevelMap> map;
    Signal<ToplevelUnmap> unmap;
    Signal<ToplevelDestroy> destroy;
    Signal<ToplevelRequestMaximize> request_maximize;
    Signal<ToplevelRequestFullscreen> request_fullscreen;
    Signal<ToplevelRequestMinimize> request_minimize;
    Signal<ToplevelRequestMove> request_move;
    Signal<ToplevelRequestResize> request_resize;
    Signal<ToplevelIdentityChange> identity_change;
    Signal<ToplevelParentChange> parent_change;
    Signal<ToplevelRequestWindowMenu> request_window_menu;

    [[nodiscard]] virtual Surface& surface() noexcept = 0;
    /// The window this one is transient for (a dialog's owner), or null.
    [[nodiscard]] virtual Toplevel* parent() const noexcept = 0;
    [[nodiscard]] virtual bool mapped() const noexcept = 0;

    [[nodiscard]] virtual const std::string& title() const noexcept = 0;
    [[nodiscard]] virtual const std::string& app_id() const noexcept = 0;
    /// Client size hints (0 = unset / unconstrained).
    [[nodiscard]] virtual int min_width() const noexcept = 0;
    [[nodiscard]] virtual int min_height() const noexcept = 0;
    [[nodiscard]] virtual int max_width() const noexcept = 0;
    [[nodiscard]] virtual int max_height() const noexcept = 0;
    /// The client's window geometry (visible bounds inside its buffer). Falls
    /// back to the whole committed buffer when the client never set one.
    [[nodiscard]] virtual XdgGeometry geometry() const noexcept = 0;

    /// Ask the client to take this size (0,0 = "you choose"). Sends the current
    /// state set along with it. Returns the configure serial.
    virtual std::uint32_t configure(int width, int height) = 0;
    /// Change a state and immediately re-configure the client at its last size.
    virtual void set_maximized(bool maximized) = 0;
    virtual void set_fullscreen(bool fullscreen) = 0;
    virtual void set_activated(bool activated) = 0;
    virtual void set_resizing(bool resizing) = 0;

    [[nodiscard]] virtual bool is_maximized() const noexcept = 0;
    [[nodiscard]] virtual bool is_fullscreen() const noexcept = 0;
    [[nodiscard]] virtual bool is_activated() const noexcept = 0;
    [[nodiscard]] virtual bool is_resizing() const noexcept = 0;
    /// Size we last asked the client to take (0 if we never asked).
    [[nodiscard]] virtual int pending_width() const noexcept = 0;
    [[nodiscard]] virtual int pending_height() const noexcept = 0;

    /// Politely ask the client to close (xdg_toplevel.close).
    virtual void close() = 0;

protected:
    Toplevel() = default;
};

struct NewPopup {
    Popup& popup;
};
struct PopupMap {
    Popup& popup;
};
struct PopupUnmap {
    Popup& popup;
};
struct PopupDestroy {
    Popup& popup;
};
/// The client re-positioned the popup (xdg_popup.reposition); x()/y() are new.
struct PopupReposition {
    Popup& popup;
};
/// The client asked for an explicit grab (menu semantics): input should be
/// funnelled to the popup, and a click elsewhere should dismiss it.
struct PopupGrab {
    Popup& popup;
    std::uint32_t serial;
};

/// A menu / tooltip / drop-down. Owned by its xdg_popup resource.
class Popup {
public:
    virtual ~Popup() = default;
    Popup(const Popup&) = delete;
    Popup& operator=(const Popup&) = delete;

    Signal<PopupMap> map;
    Signal<PopupUnmap> unmap;
    Signal<PopupDestroy> destroy;
    Signal<PopupReposition> reposition;
    Signal<PopupGrab> grab;

    [[nodiscard]] virtual Surface& surface() noexcept = 0;
    /// The parent wl_surface this popup is positioned against (may be null if
    /// the parent went away).
    [[nodiscard]] virtual Surface* parent_surface() const noexcept = 0;
    [[nodiscard]] virtual bool mapped() const noexcept = 0;
    [[nodiscard]] virtual bool has_grab() const noexcept = 0;

    /// Position relative to the parent surface's window geometry origin.
    [[nodiscard]] virtual int x() const noexcept = 0;
    [[nodiscard]] virtual int y() const noexcept = 0;
    [[nodiscard]] virtual int width() const noexcept = 0;
    [[nodiscard]] virtual int height() const noexcept = 0;

    /// Tell the client the popup is gone (click outside a grab, parent closed).
    /// The client is expected to destroy it.
    virtual void dismiss() = 0;

protected:
    Popup() = default;
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
    /// Fires when a client creates a popup.
    [[nodiscard]] Signal<NewPopup>& new_popup() noexcept;

    /// Largest window size the compositor recommends (usually the usable output
    /// area). Sent to clients as xdg_toplevel.configure_bounds. 0 disables it.
    void set_bounds(int width, int height) noexcept;

    /// Where a popup's parent sits, so xdg_positioner's constraint_adjustment
    /// (flip / slide / resize) can actually be applied: fill `parent` with the
    /// parent surface's window-geometry origin in the coordinate space you lay
    /// windows out in, and `usable` with the area a popup must stay inside
    /// (normally the output the parent is on). Return false if the surface isn't
    /// placed anywhere yet.
    ///
    /// Without this the shell has no idea where on screen the parent is, so a
    /// menu near an edge overhangs instead of flipping. Set it and menus behave.
    using PopupConstraintQuery = std::function<bool(Surface& parent, Box& parent_box, Box& usable)>;
    void set_popup_constraint_query(PopupConstraintQuery query);

    struct Impl; // named by the protocol glue

private:
    std::unique_ptr<Impl> impl_;
    explicit XdgShell(std::unique_ptr<Impl> impl) noexcept;
};

/// The Toplevel behind an `xdg_toplevel` resource, or null if that resource is
/// something else. How other globals (xdg-decoration) get from a client object
/// to ours.
[[nodiscard]] Toplevel* toplevel_from_resource(wl_resource* resource);

} // namespace luminaria
