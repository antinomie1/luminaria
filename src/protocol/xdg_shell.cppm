// luminaria/xdg_shell.cppm — xdg-shell: application windows (toplevels) and popups.
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

module;

#include "detail/wayland_fwd.h"
#include <cstdint>
#include <typeinfo>

#include <wayland-server-core.h>
#include "xdg-shell-protocol.h"

export module luminaria:xdg_shell;

import std;

import :box;
import :compositor;
import :display;
import :expected;
import :protocol_helper;
import :signal;

export namespace luminaria {

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
/// The COMPOSITOR changed this window's state (maximized / fullscreen /
/// activated / resizing / minimized) — i.e. one of the set_* methods below
/// actually changed something. Distinct from the request_* signals above, which
/// are the client asking. Window-list protocols mirror this.
struct ToplevelStateChange {
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
    Signal<ToplevelStateChange> state_change;
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
    /// "Your edges are against something, so take the size I give you exactly."
    ///
    /// All four edges at once, because that is the only distinction a tiling
    /// compositor is making: this window does not choose its own size. It is
    /// what stops a terminal rounding the height down to a whole cell and
    /// leaving a strip of background inside its border, and what makes a
    /// client drop the rounded corners and drop shadow it draws for a free
    /// floating window. Sent only to clients on xdg_toplevel version 2+.
    virtual void set_tiled(bool tiled) = 0;
    /// Compositor bookkeeping only: xdg-shell has no minimized state, so nothing
    /// is sent to the client (minimizing a window means not drawing it). It is
    /// tracked here because window-list protocols and taskbars need it, and
    /// because `request_minimize` has to land somewhere.
    virtual void set_minimized(bool minimized) = 0;

    [[nodiscard]] virtual bool is_maximized() const noexcept = 0;
    [[nodiscard]] virtual bool is_fullscreen() const noexcept = 0;
    [[nodiscard]] virtual bool is_activated() const noexcept = 0;
    [[nodiscard]] virtual bool is_resizing() const noexcept = 0;
    [[nodiscard]] virtual bool is_tiled() const noexcept = 0;
    [[nodiscard]] virtual bool is_minimized() const noexcept = 0;
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

/// The same for an `xdg_popup` resource. layer-shell needs it: a panel's menu is
/// an xdg_popup re-parented to a zwlr_layer_surface_v1.
[[nodiscard]] Popup* popup_from_resource(wl_resource* resource);

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct XdgShell::Impl {
    wl_display* display = nullptr;
    WlGlobal global;
    Signal<NewToplevel> new_toplevel;
    Signal<NewPopup> new_popup;
    int bounds_width = 0;
    int bounds_height = 0;
    XdgShell::PopupConstraintQuery constraint_query;
};

namespace {

struct XdgSurface;
class PopupImpl;

// ---------------------------------------------------------------- positioner

// The full xdg_positioner state (protocol version 3). `place()` turns it into a
// popup rectangle relative to the parent's window-geometry origin.
struct Positioner {
    int width = 0, height = 0;
    int anchor_x = 0, anchor_y = 0, anchor_width = 0, anchor_height = 0;
    uint32_t anchor = XDG_POSITIONER_ANCHOR_NONE;
    uint32_t gravity = XDG_POSITIONER_GRAVITY_NONE;
    uint32_t constraint_adjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_NONE;
    int offset_x = 0, offset_y = 0;
    bool reactive = false;
    int parent_width = 0, parent_height = 0;
    uint32_t parent_configure = 0;
};

bool anchor_is_top(uint32_t a) {
    return a == XDG_POSITIONER_ANCHOR_TOP || a == XDG_POSITIONER_ANCHOR_TOP_LEFT ||
           a == XDG_POSITIONER_ANCHOR_TOP_RIGHT;
}
bool anchor_is_bottom(uint32_t a) {
    return a == XDG_POSITIONER_ANCHOR_BOTTOM || a == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT ||
           a == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
}
bool anchor_is_left(uint32_t a) {
    return a == XDG_POSITIONER_ANCHOR_LEFT || a == XDG_POSITIONER_ANCHOR_TOP_LEFT ||
           a == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
}
bool anchor_is_right(uint32_t a) {
    return a == XDG_POSITIONER_ANCHOR_RIGHT || a == XDG_POSITIONER_ANCHOR_TOP_RIGHT ||
           a == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
}
// The gravity enum has the same layout as the anchor enum, so the same tests work.
static_assert(static_cast<uint32_t>(XDG_POSITIONER_GRAVITY_TOP_LEFT) ==
              static_cast<uint32_t>(XDG_POSITIONER_ANCHOR_TOP_LEFT));

/// Mirror an anchor/gravity value along one axis. Flipping a popup means
/// flipping both, so a menu anchored bottom-left growing down becomes anchored
/// top-left growing up.
uint32_t flip_axis(uint32_t v, bool horizontal) {
    switch (v) {
    case XDG_POSITIONER_ANCHOR_TOP:
        return horizontal ? v : uint32_t{XDG_POSITIONER_ANCHOR_BOTTOM};
    case XDG_POSITIONER_ANCHOR_BOTTOM:
        return horizontal ? v : uint32_t{XDG_POSITIONER_ANCHOR_TOP};
    case XDG_POSITIONER_ANCHOR_LEFT:
        return horizontal ? uint32_t{XDG_POSITIONER_ANCHOR_RIGHT} : v;
    case XDG_POSITIONER_ANCHOR_RIGHT:
        return horizontal ? uint32_t{XDG_POSITIONER_ANCHOR_LEFT} : v;
    case XDG_POSITIONER_ANCHOR_TOP_LEFT:
        return horizontal ? XDG_POSITIONER_ANCHOR_TOP_RIGHT : XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
    case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
        return horizontal ? XDG_POSITIONER_ANCHOR_TOP_LEFT : XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
    case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
        return horizontal ? XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT : XDG_POSITIONER_ANCHOR_TOP_LEFT;
    case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
        return horizontal ? XDG_POSITIONER_ANCHOR_BOTTOM_LEFT : XDG_POSITIONER_ANCHOR_TOP_RIGHT;
    default:
        return v;
    }
}

/// Anchor point on the anchor rect, then the popup laid out from it per gravity,
/// then the client's offset. All in parent window-geometry coordinates.
XdgGeometry place(const Positioner& p) {
    int x = p.anchor_x;
    int y = p.anchor_y;
    if (anchor_is_left(p.anchor)) {
        // x += 0
    } else if (anchor_is_right(p.anchor)) {
        x += p.anchor_width;
    } else {
        x += p.anchor_width / 2;
    }
    if (anchor_is_top(p.anchor)) {
        // y += 0
    } else if (anchor_is_bottom(p.anchor)) {
        y += p.anchor_height;
    } else {
        y += p.anchor_height / 2;
    }

    // Gravity is the direction the popup extends away from the anchor point.
    if (anchor_is_left(p.gravity)) {
        x -= p.width;
    } else if (!anchor_is_right(p.gravity)) {
        x -= p.width / 2;
    }
    if (anchor_is_top(p.gravity)) {
        y -= p.height;
    } else if (!anchor_is_bottom(p.gravity)) {
        y -= p.height / 2;
    }

    return XdgGeometry{x + p.offset_x, y + p.offset_y, p.width, p.height};
}

/// Apply constraint_adjustment. `geo` comes out of place() in parent
/// window-geometry coordinates; `parent_box` says where that origin is in the
/// layout, and `usable` is the area the popup has to stay inside (the output).
///
/// The protocol fixes the order: flip first (a menu that would fall off the
/// bottom opens upwards), then slide (nudge it back in), then resize (shrink it
/// as a last resort). Each axis is solved independently.
XdgGeometry constrain(const Positioner& p, XdgGeometry geo, const Box& parent_box,
                      const Box& usable) {
    const uint32_t adjust = p.constraint_adjustment;
    if (adjust == XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_NONE || usable.empty()) {
        return geo;
    }
    // Work in layout coordinates, then convert back at the end.
    const int ox = parent_box.x;
    const int oy = parent_box.y;
    auto overflows_x = [&](const XdgGeometry& g) {
        return g.x + ox < usable.x || g.x + ox + g.width > usable.x + usable.width;
    };
    auto overflows_y = [&](const XdgGeometry& g) {
        return g.y + oy < usable.y || g.y + oy + g.height > usable.y + usable.height;
    };

    if ((adjust & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X) != 0 && overflows_x(geo)) {
        Positioner flipped = p;
        flipped.anchor = flip_axis(p.anchor, true);
        flipped.gravity = flip_axis(p.gravity, true);
        flipped.offset_x = -p.offset_x;
        const XdgGeometry candidate = place(flipped);
        if (!overflows_x(candidate)) {
            geo.x = candidate.x; // only take the flip if it actually helped
        }
    }
    if ((adjust & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y) != 0 && overflows_y(geo)) {
        Positioner flipped = p;
        flipped.anchor = flip_axis(p.anchor, false);
        flipped.gravity = flip_axis(p.gravity, false);
        flipped.offset_y = -p.offset_y;
        const XdgGeometry candidate = place(flipped);
        if (!overflows_y(candidate)) {
            geo.y = candidate.y;
        }
    }
    if ((adjust & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X) != 0 && overflows_x(geo)) {
        // Push the right edge in first, then the left: with a popup wider than
        // the screen the left edge is the one that has to win.
        if (geo.x + ox + geo.width > usable.x + usable.width) {
            geo.x = usable.x + usable.width - geo.width - ox;
        }
        if (geo.x + ox < usable.x) {
            geo.x = usable.x - ox;
        }
    }
    if ((adjust & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y) != 0 && overflows_y(geo)) {
        if (geo.y + oy + geo.height > usable.y + usable.height) {
            geo.y = usable.y + usable.height - geo.height - oy;
        }
        if (geo.y + oy < usable.y) {
            geo.y = usable.y - oy;
        }
    }
    if ((adjust & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X) != 0 && overflows_x(geo)) {
        const int left = std::max(geo.x + ox, usable.x);
        const int right = std::min(geo.x + ox + geo.width, usable.x + usable.width);
        geo.x = left - ox;
        geo.width = std::max(1, right - left);
    }
    if ((adjust & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y) != 0 && overflows_y(geo)) {
        const int top = std::max(geo.y + oy, usable.y);
        const int bottom = std::min(geo.y + oy + geo.height, usable.y + usable.height);
        geo.y = top - oy;
        geo.height = std::max(1, bottom - top);
    }
    return geo;
}

// ------------------------------------------------------------------ toplevel

// Concrete toplevel. Owned by its own xdg_toplevel wl_resource (address stable
// for its lifetime). `owner` links back to the XdgSurface; either side nulls the
// cross-pointer when it dies, so client teardown in any resource order is safe.
class ToplevelImpl final : public Toplevel {
public:
    Surface* surf = nullptr;
    wl_resource* resource = nullptr; // xdg_toplevel
    XdgSurface* owner = nullptr;     // nulled if the xdg_surface is destroyed first
    bool mapped_ = false;

    std::string title_;
    std::string app_id_;
    int min_w_ = 0, min_h_ = 0, max_w_ = 0, max_h_ = 0;
    bool maximized_ = false, fullscreen_ = false, activated_ = false, resizing_ = false;
    bool tiled_ = false;
    bool minimized_ = false;
    int pending_w_ = 0, pending_h_ = 0;

    ToplevelImpl* parent_ = nullptr;              // transient-for
    std::vector<ToplevelImpl*> transients_;       // the other end of that edge

    ~ToplevelImpl() override {
        // Unlink both directions so no half of a transient pair dangles.
        set_parent_impl(nullptr);
        for (ToplevelImpl* child : transients_) {
            child->parent_ = nullptr;
            ToplevelParentChange event{*child, nullptr};
            child->parent_change.emit(event);
        }
        transients_.clear();
    }

    void set_parent_impl(ToplevelImpl* parent) {
        if (parent_ == parent) {
            return;
        }
        if (parent_ != nullptr) {
            std::erase(parent_->transients_, this);
        }
        parent_ = parent;
        if (parent_ != nullptr) {
            parent_->transients_.push_back(this);
        }
        ToplevelParentChange event{*this, parent_};
        parent_change.emit(event);
    }

    Surface& surface() noexcept override { return *surf; }
    [[nodiscard]] Toplevel* parent() const noexcept override { return parent_; }
    [[nodiscard]] bool mapped() const noexcept override { return mapped_; }
    [[nodiscard]] const std::string& title() const noexcept override { return title_; }
    [[nodiscard]] const std::string& app_id() const noexcept override { return app_id_; }
    [[nodiscard]] int min_width() const noexcept override { return min_w_; }
    [[nodiscard]] int min_height() const noexcept override { return min_h_; }
    [[nodiscard]] int max_width() const noexcept override { return max_w_; }
    [[nodiscard]] int max_height() const noexcept override { return max_h_; }
    [[nodiscard]] XdgGeometry geometry() const noexcept override;

    uint32_t configure(int width, int height) override;
    // Each of these re-configures the client at its last known size, but only
    // when the state actually changed — a no-op set_activated(true) on an
    // already-focused window shouldn't make it repaint.
    void reconfigure_and_announce() {
        (void)configure(pending_w_, pending_h_);
        announce_state();
    }
    void announce_state() {
        ToplevelStateChange event{*this};
        state_change.emit(event);
    }
    void set_maximized(bool v) override {
        if (maximized_ == v) {
            return;
        }
        maximized_ = v;
        reconfigure_and_announce();
    }
    void set_fullscreen(bool v) override {
        if (fullscreen_ == v) {
            return;
        }
        fullscreen_ = v;
        reconfigure_and_announce();
    }
    void set_activated(bool v) override {
        if (activated_ == v) {
            return;
        }
        activated_ = v;
        reconfigure_and_announce();
    }
    void set_resizing(bool v) override {
        if (resizing_ == v) {
            return;
        }
        resizing_ = v;
        reconfigure_and_announce();
    }
    void set_tiled(bool v) override {
        if (tiled_ == v) {
            return;
        }
        tiled_ = v;
        reconfigure_and_announce();
    }
    // No configure: xdg-shell has no minimized state to send.
    void set_minimized(bool v) override {
        if (minimized_ == v) {
            return;
        }
        minimized_ = v;
        announce_state();
    }
    [[nodiscard]] bool is_maximized() const noexcept override { return maximized_; }
    [[nodiscard]] bool is_fullscreen() const noexcept override { return fullscreen_; }
    [[nodiscard]] bool is_activated() const noexcept override { return activated_; }
    [[nodiscard]] bool is_resizing() const noexcept override { return resizing_; }
    [[nodiscard]] bool is_tiled() const noexcept override { return tiled_; }
    [[nodiscard]] bool is_minimized() const noexcept override { return minimized_; }
    [[nodiscard]] int pending_width() const noexcept override { return pending_w_; }
    [[nodiscard]] int pending_height() const noexcept override { return pending_h_; }

    void close() override {
        if (resource != nullptr) {
            xdg_toplevel_send_close(resource);
        }
    }
};

// --------------------------------------------------------------------- popup

class PopupImpl final : public Popup {
public:
    Surface* surf = nullptr;
    wl_resource* resource = nullptr; // xdg_popup
    XdgSurface* owner = nullptr;     // our own xdg_surface; nulled if it dies first
    XdgSurface* parent = nullptr;    // parent xdg_surface; nulled if it dies first
    Positioner positioner;
    XdgGeometry geo;
    bool mapped_ = false;
    bool grabbed_ = false;
    bool done_ = false;

    Surface& surface() noexcept override { return *surf; }
    [[nodiscard]] Surface* parent_surface() const noexcept override;
    [[nodiscard]] bool mapped() const noexcept override { return mapped_; }
    [[nodiscard]] bool has_grab() const noexcept override { return grabbed_; }
    [[nodiscard]] int x() const noexcept override { return geo.x; }
    [[nodiscard]] int y() const noexcept override { return geo.y; }
    [[nodiscard]] int width() const noexcept override { return geo.width; }
    [[nodiscard]] int height() const noexcept override { return geo.height; }

    void dismiss() override;
    void send_configure();
};

// --------------------------------------------------------------- xdg_surface

// Per-xdg_surface driver. Owned by the xdg_surface resource.
struct XdgSurface {
    XdgShell::Impl* shell = nullptr;
    wl_resource* resource = nullptr; // xdg_surface
    Surface* surface = nullptr;
    Signal<SurfaceCommit>::Connection commit_conn;
    ToplevelImpl* toplevel = nullptr; // owned by its resource; nulled on its destroy
    PopupImpl* popup = nullptr;       // ditto
    std::vector<PopupImpl*> child_popups; // popups parented to this xdg_surface
    bool initialized = false;
    bool has_geometry = false;
    XdgGeometry geometry;
    uint32_t last_acked = 0;

    [[nodiscard]] XdgGeometry effective_geometry() const {
        if (has_geometry) {
            return geometry;
        }
        // xdg_surface geometry is in surface coordinates, not buffer pixels: a
        // HiDPI client's 2x buffer still describes a 1x window.
        return XdgGeometry{0, 0, surface != nullptr ? surface->surface_width() : 0,
                           surface != nullptr ? surface->surface_height() : 0};
    }
};

XdgGeometry ToplevelImpl::geometry() const noexcept {
    if (owner != nullptr) {
        return owner->effective_geometry();
    }
    return XdgGeometry{};
}

Surface* PopupImpl::parent_surface() const noexcept {
    return parent != nullptr ? parent->surface : nullptr;
}

/// place() plus the constraint pass, when the compositor told us where things
/// are. Every popup position goes through here, so create and reposition can't
/// drift apart.
XdgGeometry place_popup(PopupImpl& popup) {
    const XdgGeometry geo = place(popup.positioner);
    XdgShell::Impl* shell = popup.parent != nullptr ? popup.parent->shell : nullptr;
    Surface* parent_surface = popup.parent_surface();
    if (shell == nullptr || !shell->constraint_query || parent_surface == nullptr) {
        return geo;
    }
    Box parent_box{};
    Box usable{};
    if (!shell->constraint_query(*parent_surface, parent_box, usable)) {
        return geo;
    }
    return constrain(popup.positioner, geo, parent_box, usable);
}

uint32_t ToplevelImpl::configure(int width, int height) {
    if (owner == nullptr || resource == nullptr) {
        return 0;
    }
    // A configure carrying MAXIMIZED or FULLSCREEN must name a real size: the
    // client is required to obey it, so 0x0 ("you pick") is a contradiction.
    // Qt logs `Configure event with maximized or fullscreen state contains
    // invalid width: 0` and ignores the whole event. Substitute the best size we
    // have — the compositor's advertised bounds, else the window's own geometry.
    if ((maximized_ || fullscreen_) && (width <= 0 || height <= 0)) {
        const XdgGeometry geo = owner->effective_geometry();
        width = owner->shell->bounds_width > 0 ? owner->shell->bounds_width : geo.width;
        height = owner->shell->bounds_height > 0 ? owner->shell->bounds_height : geo.height;
    }
    const uint32_t version = static_cast<uint32_t>(wl_resource_get_version(resource));
    if (version >= XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION && owner->shell->bounds_width > 0) {
        xdg_toplevel_send_configure_bounds(resource, owner->shell->bounds_width,
                                           owner->shell->bounds_height);
    }
    wl_array states;
    wl_array_init(&states);
    const auto push = [&states](uint32_t state) {
        if (auto* slot = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
            slot != nullptr) {
            *slot = state;
        }
    };
    if (maximized_) {
        push(XDG_TOPLEVEL_STATE_MAXIMIZED);
    }
    if (fullscreen_) {
        push(XDG_TOPLEVEL_STATE_FULLSCREEN);
    }
    if (resizing_) {
        push(XDG_TOPLEVEL_STATE_RESIZING);
    }
    if (activated_) {
        push(XDG_TOPLEVEL_STATE_ACTIVATED);
    }
    // Since version 2, and a client on version 1 must not be sent a state it
    // has no enum value for.
    if (tiled_ && version >= XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION) {
        push(XDG_TOPLEVEL_STATE_TILED_LEFT);
        push(XDG_TOPLEVEL_STATE_TILED_RIGHT);
        push(XDG_TOPLEVEL_STATE_TILED_TOP);
        push(XDG_TOPLEVEL_STATE_TILED_BOTTOM);
    }
    xdg_toplevel_send_configure(resource, width, height, &states);
    wl_array_release(&states);
    pending_w_ = width;
    pending_h_ = height;

    const uint32_t serial = wl_display_next_serial(owner->shell->display);
    xdg_surface_send_configure(owner->resource, serial);
    return serial;
}

void PopupImpl::send_configure() {
    if (owner == nullptr || resource == nullptr) {
        return;
    }
    xdg_popup_send_configure(resource, geo.x, geo.y, geo.width, geo.height);
    const uint32_t serial = wl_display_next_serial(owner->shell->display);
    xdg_surface_send_configure(owner->resource, serial);
}

void PopupImpl::dismiss() {
    if (done_ || resource == nullptr) {
        return;
    }
    done_ = true;
    // Children first: a nested submenu must go away before its parent.
    if (owner != nullptr) {
        const std::vector<PopupImpl*> children = owner->child_popups;
        for (PopupImpl* child : children) {
            child->dismiss();
        }
    }
    xdg_popup_send_popup_done(resource);
    if (mapped_) {
        mapped_ = false;
        PopupUnmap event{*this};
        unmap.emit(event);
    }
}

void on_surface_commit(XdgSurface* xs) {
    if (!xs->initialized) {
        // Initial commit: the client is waiting for its first configure.
        xs->initialized = true;
        if (xs->toplevel != nullptr) {
            (void)xs->toplevel->configure(0, 0);
        } else if (xs->popup != nullptr) {
            xs->popup->send_configure();
        } else {
            // Role not assigned yet — still answer, per xdg_surface semantics.
            xdg_surface_send_configure(xs->resource, wl_display_next_serial(xs->shell->display));
        }
        return;
    }
    if (xs->toplevel != nullptr) {
        ToplevelImpl* tl = xs->toplevel;
        if (xs->surface->has_buffer() && !tl->mapped_) {
            tl->mapped_ = true;
            ToplevelMap event{*tl};
            tl->map.emit(event);
        } else if (!xs->surface->has_buffer() && tl->mapped_) {
            tl->mapped_ = false;
            ToplevelUnmap event{*tl};
            tl->unmap.emit(event);
        }
        return;
    }
    if (xs->popup != nullptr) {
        PopupImpl* popup = xs->popup;
        if (xs->surface->has_buffer() && !popup->mapped_ && !popup->done_) {
            popup->mapped_ = true;
            PopupMap event{*popup};
            popup->map.emit(event);
        } else if (!xs->surface->has_buffer() && popup->mapped_) {
            popup->mapped_ = false;
            PopupUnmap event{*popup};
            popup->unmap.emit(event);
        }
    }
}

// ---- xdg_toplevel requests ----
ToplevelImpl* toplevel_of(wl_resource* resource) {
    return static_cast<ToplevelImpl*>(wl_resource_get_user_data(resource));
}
// Transient-for. The shell tracks the relationship (and keeps it consistent
// when either window dies); stacking and centring are the compositor's call, so
// it hears about it through the signal.
void tl_set_parent(wl_client*, wl_resource* resource, wl_resource* parent_resource) {
    ToplevelImpl* tl = toplevel_of(resource);
    auto* parent = parent_resource != nullptr ? toplevel_of(parent_resource) : nullptr;
    if (parent == tl) {
        return; // a window cannot be its own parent
    }
    tl->set_parent_impl(parent);
}
// The client wants the compositor's window menu at (x,y). We have no menu of our
// own, so this is purely a signal — a compositor that wants one draws it.
void tl_show_window_menu(wl_client*, wl_resource* resource, wl_resource*, uint32_t serial,
                         int32_t x, int32_t y) {
    ToplevelImpl* tl = toplevel_of(resource);
    ToplevelRequestWindowMenu event{*tl, serial, x, y};
    tl->request_window_menu.emit(event);
}
void tl_set_title(wl_client*, wl_resource* resource, const char* title) {
    ToplevelImpl* tl = toplevel_of(resource);
    tl->title_ = title != nullptr ? title : "";
    ToplevelIdentityChange event{*tl};
    tl->identity_change.emit(event);
}
void tl_set_app_id(wl_client*, wl_resource* resource, const char* app_id) {
    ToplevelImpl* tl = toplevel_of(resource);
    tl->app_id_ = app_id != nullptr ? app_id : "";
    ToplevelIdentityChange event{*tl};
    tl->identity_change.emit(event);
}
void tl_move(wl_client*, wl_resource* resource, wl_resource*, uint32_t serial) {
    ToplevelImpl* tl = toplevel_of(resource);
    ToplevelRequestMove event{*tl, serial};
    tl->request_move.emit(event);
}
void tl_resize(wl_client*, wl_resource* resource, wl_resource*, uint32_t serial, uint32_t edges) {
    ToplevelImpl* tl = toplevel_of(resource);
    ToplevelRequestResize event{*tl, serial, edges};
    tl->request_resize.emit(event);
}
void tl_set_max_size(wl_client*, wl_resource* resource, int32_t width, int32_t height) {
    ToplevelImpl* tl = toplevel_of(resource);
    tl->max_w_ = width;
    tl->max_h_ = height;
}
void tl_set_min_size(wl_client*, wl_resource* resource, int32_t width, int32_t height) {
    ToplevelImpl* tl = toplevel_of(resource);
    tl->min_w_ = width;
    tl->min_h_ = height;
}
void tl_request_maximized(wl_resource* resource, bool maximized) {
    ToplevelImpl* tl = toplevel_of(resource);
    ToplevelRequestMaximize event{*tl, maximized};
    tl->request_maximize.emit(event);
    if (tl->maximized_ != maximized && tl->request_maximize.slot_count() == 0) {
        // Nobody is arbitrating; ack the request so the client isn't left hanging.
        tl->set_maximized(maximized);
    }
}
void tl_set_maximized(wl_client*, wl_resource* resource) {
    tl_request_maximized(resource, true);
}
void tl_unset_maximized(wl_client*, wl_resource* resource) {
    tl_request_maximized(resource, false);
}
void tl_request_fullscreen(wl_resource* resource, bool fullscreen) {
    ToplevelImpl* tl = toplevel_of(resource);
    ToplevelRequestFullscreen event{*tl, fullscreen};
    tl->request_fullscreen.emit(event);
    if (tl->fullscreen_ != fullscreen && tl->request_fullscreen.slot_count() == 0) {
        tl->set_fullscreen(fullscreen);
    }
}
void tl_set_fullscreen(wl_client*, wl_resource* resource, wl_resource*) {
    tl_request_fullscreen(resource, true);
}
void tl_unset_fullscreen(wl_client*, wl_resource* resource) {
    tl_request_fullscreen(resource, false);
}
void tl_set_minimized(wl_client*, wl_resource* resource) {
    ToplevelImpl* tl = toplevel_of(resource);
    ToplevelRequestMinimize event{*tl};
    tl->request_minimize.emit(event);
    if (!tl->minimized_ && tl->request_minimize.slot_count() == 0) {
        // Match maximize/fullscreen: with no compositor policy listening, at
        // least retain the state so window-list consumers see the request.
        tl->set_minimized(true);
    }
}
constexpr struct xdg_toplevel_interface toplevel_impl = {
    .destroy = resource_destroy_request,
    .set_parent = tl_set_parent,
    .set_title = tl_set_title,
    .set_app_id = tl_set_app_id,
    .show_window_menu = tl_show_window_menu,
    .move = tl_move,
    .resize = tl_resize,
    .set_max_size = tl_set_max_size,
    .set_min_size = tl_set_min_size,
    .set_maximized = tl_set_maximized,
    .unset_maximized = tl_unset_maximized,
    .set_fullscreen = tl_set_fullscreen,
    .unset_fullscreen = tl_unset_fullscreen,
    .set_minimized = tl_set_minimized,
};
void toplevel_resource_destroy(wl_resource* resource) {
    auto* tl = toplevel_of(resource);
    if (tl->mapped_) {
        tl->mapped_ = false;
        ToplevelUnmap unmap_event{*tl};
        tl->unmap.emit(unmap_event);
    }
    ToplevelDestroy event{*tl};
    tl->destroy.emit(event);
    if (tl->owner != nullptr) {
        tl->owner->toplevel = nullptr; // stop the xdg_surface from touching us
    }
    delete tl;
}

// ---- xdg_positioner ----
Positioner* positioner_of(wl_resource* resource) {
    return static_cast<Positioner*>(wl_resource_get_user_data(resource));
}
void pos_set_size(wl_client*, wl_resource* resource, int32_t width, int32_t height) {
    if (width < 1 || height < 1) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
                               "set_size: width and height must be positive");
        return;
    }
    positioner_of(resource)->width = width;
    positioner_of(resource)->height = height;
}
void pos_set_anchor_rect(wl_client*, wl_resource* resource, int32_t x, int32_t y, int32_t width,
                         int32_t height) {
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
                               "set_anchor_rect: negative size");
        return;
    }
    Positioner* p = positioner_of(resource);
    p->anchor_x = x;
    p->anchor_y = y;
    p->anchor_width = width;
    p->anchor_height = height;
}
void pos_set_anchor(wl_client*, wl_resource* resource, uint32_t anchor) {
    positioner_of(resource)->anchor = anchor;
}
void pos_set_gravity(wl_client*, wl_resource* resource, uint32_t gravity) {
    positioner_of(resource)->gravity = gravity;
}
void pos_set_constraint_adjustment(wl_client*, wl_resource* resource, uint32_t adjustment) {
    positioner_of(resource)->constraint_adjustment = adjustment;
}
void pos_set_offset(wl_client*, wl_resource* resource, int32_t x, int32_t y) {
    positioner_of(resource)->offset_x = x;
    positioner_of(resource)->offset_y = y;
}
void pos_set_reactive(wl_client*, wl_resource* resource) {
    positioner_of(resource)->reactive = true;
}
void pos_set_parent_size(wl_client*, wl_resource* resource, int32_t width, int32_t height) {
    positioner_of(resource)->parent_width = width;
    positioner_of(resource)->parent_height = height;
}
void pos_set_parent_configure(wl_client*, wl_resource* resource, uint32_t serial) {
    positioner_of(resource)->parent_configure = serial;
}
constexpr struct xdg_positioner_interface positioner_impl = {
    .destroy = resource_destroy_request,
    .set_size = pos_set_size,
    .set_anchor_rect = pos_set_anchor_rect,
    .set_anchor = pos_set_anchor,
    .set_gravity = pos_set_gravity,
    .set_constraint_adjustment = pos_set_constraint_adjustment,
    .set_offset = pos_set_offset,
    .set_reactive = pos_set_reactive,
    .set_parent_size = pos_set_parent_size,
    .set_parent_configure = pos_set_parent_configure,
};
void positioner_resource_destroy(wl_resource* resource) {
    delete positioner_of(resource);
}

// ---- xdg_popup requests ----
PopupImpl* popup_of(wl_resource* resource) {
    return static_cast<PopupImpl*>(wl_resource_get_user_data(resource));
}
void popup_grab(wl_client*, wl_resource* resource, wl_resource*, uint32_t serial) {
    PopupImpl* popup = popup_of(resource);
    popup->grabbed_ = true;
    PopupGrab event{*popup, serial};
    popup->grab.emit(event);
}
void popup_reposition(wl_client*, wl_resource* resource, wl_resource* positioner_resource,
                      uint32_t token) {
    PopupImpl* popup = popup_of(resource);
    popup->positioner = *positioner_of(positioner_resource);
    popup->geo = place_popup(*popup);
    if (wl_resource_get_version(resource) >= XDG_POPUP_REPOSITIONED_SINCE_VERSION) {
        xdg_popup_send_repositioned(resource, token);
    }
    popup->send_configure();
    PopupReposition event{*popup};
    popup->reposition.emit(event);
}
constexpr struct xdg_popup_interface popup_impl = {
    .destroy = resource_destroy_request,
    .grab = popup_grab,
    .reposition = popup_reposition,
};
void popup_resource_destroy(wl_resource* resource) {
    auto* popup = popup_of(resource);
    if (popup->mapped_) {
        popup->mapped_ = false;
        PopupUnmap unmap_event{*popup};
        popup->unmap.emit(unmap_event);
    }
    PopupDestroy event{*popup};
    popup->destroy.emit(event);
    if (popup->owner != nullptr) {
        popup->owner->popup = nullptr;
    }
    if (popup->parent != nullptr) {
        std::erase(popup->parent->child_popups, popup);
    }
    delete popup;
}

// ---- xdg_surface ----
XdgSurface* xdg_surface_of(wl_resource* resource) {
    return static_cast<XdgSurface*>(wl_resource_get_user_data(resource));
}
void xdg_surface_get_toplevel(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* xs = xdg_surface_of(resource);
    if (xs->toplevel != nullptr || xs->popup != nullptr) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                               "xdg_surface already has a role");
        return;
    }
    auto* tl = new ToplevelImpl();
    tl->surf = xs->surface;
    tl->owner = xs;
    tl->resource = wl_resource_create(client, &xdg_toplevel_interface,
                                      wl_resource_get_version(resource), id);
    if (tl->resource == nullptr) {
        delete tl;
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(tl->resource, &toplevel_impl, tl, toplevel_resource_destroy);
    xs->toplevel = tl;

    // Tell the client which window-management actions we honour (v5+).
    if (wl_resource_get_version(tl->resource) >= XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION) {
        wl_array caps;
        wl_array_init(&caps);
        for (uint32_t cap : {static_cast<uint32_t>(XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE),
                             static_cast<uint32_t>(XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN),
                             static_cast<uint32_t>(XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE)}) {
            if (auto* slot = static_cast<uint32_t*>(wl_array_add(&caps, sizeof(uint32_t)));
                slot != nullptr) {
                *slot = cap;
            }
        }
        xdg_toplevel_send_wm_capabilities(tl->resource, &caps);
        wl_array_release(&caps);
    }

    NewToplevel event{*tl};
    xs->shell->new_toplevel.emit(event);
}
void xdg_surface_get_popup(wl_client* client, wl_resource* resource, uint32_t id,
                           wl_resource* parent_resource, wl_resource* positioner_resource) {
    auto* xs = xdg_surface_of(resource);
    if (xs->toplevel != nullptr || xs->popup != nullptr) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                               "xdg_surface already has a role");
        return;
    }
    Positioner* positioner = positioner_of(positioner_resource);
    if (positioner->width < 1 || positioner->height < 1) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
                               "get_popup: positioner has no size");
        return;
    }
    auto* popup = new PopupImpl();
    popup->surf = xs->surface;
    popup->owner = xs;
    popup->parent = parent_resource != nullptr ? xdg_surface_of(parent_resource) : nullptr;
    popup->positioner = *positioner;
    popup->geo = place_popup(*popup);
    popup->resource = wl_resource_create(client, &xdg_popup_interface,
                                         wl_resource_get_version(resource), id);
    if (popup->resource == nullptr) {
        delete popup;
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(popup->resource, &popup_impl, popup, popup_resource_destroy);
    xs->popup = popup;
    if (popup->parent != nullptr) {
        popup->parent->child_popups.push_back(popup);
    }

    NewPopup event{*popup};
    xs->shell->new_popup.emit(event);
}
void xdg_surface_set_window_geometry(wl_client*, wl_resource* resource, int32_t x, int32_t y,
                                     int32_t width, int32_t height) {
    auto* xs = xdg_surface_of(resource);
    xs->has_geometry = width > 0 && height > 0;
    xs->geometry = XdgGeometry{x, y, width, height};
}
void xdg_surface_ack_configure(wl_client*, wl_resource* resource, uint32_t serial) {
    xdg_surface_of(resource)->last_acked = serial;
}
constexpr struct xdg_surface_interface xdg_surface_impl = {
    .destroy = resource_destroy_request,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};
void xdg_surface_resource_destroy(wl_resource* resource) {
    auto* xs = xdg_surface_of(resource);
    if (xs->toplevel != nullptr) {
        // Our toplevel's resource outlives us; sever the back-pointer so its
        // destroy handler won't touch this freed XdgSurface.
        xs->toplevel->owner = nullptr;
    }
    if (xs->popup != nullptr) {
        xs->popup->owner = nullptr;
    }
    // Popups parented to us lose their anchor: tell them and unlink.
    const std::vector<PopupImpl*> children = xs->child_popups;
    for (PopupImpl* child : children) {
        child->dismiss();
        child->parent = nullptr;
    }
    delete xs;
}

// ---- xdg_wm_base ----
void wm_base_get_xdg_surface(wl_client* client, wl_resource* wm_resource, uint32_t id,
                             wl_resource* surface_resource) {
    auto* shell = static_cast<XdgShell::Impl*>(wl_resource_get_user_data(wm_resource));
    auto* surface = static_cast<Surface*>(wl_resource_get_user_data(surface_resource));

    auto* xs = new XdgSurface{};
    xs->shell = shell;
    xs->surface = surface;
    xs->resource = wl_resource_create(client, &xdg_surface_interface,
                                      wl_resource_get_version(wm_resource), id);
    if (xs->resource == nullptr) {
        delete xs;
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(xs->resource, &xdg_surface_impl, xs,
                                   xdg_surface_resource_destroy);
    xs->commit_conn = surface->commit.connect([xs](SurfaceCommit&) { on_surface_commit(xs); });
}
void wm_base_create_positioner(wl_client* client, wl_resource* resource, uint32_t id) {
    wl_resource* pos = wl_resource_create(client, &xdg_positioner_interface,
                                          wl_resource_get_version(resource), id);
    if (pos == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(pos, &positioner_impl, new Positioner{},
                                   positioner_resource_destroy);
}
void wm_base_pong(wl_client*, wl_resource*, uint32_t) {}
constexpr struct xdg_wm_base_interface wm_base_impl = {
    .destroy = resource_destroy_request,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = wm_base_pong,
};

} // namespace

XdgShell::XdgShell(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
XdgShell::~XdgShell() = default;
XdgShell::XdgShell(XdgShell&&) noexcept = default;
XdgShell& XdgShell::operator=(XdgShell&&) noexcept = default;

Result<XdgShell> XdgShell::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    // Version 5: popups with reposition (v3), configure_bounds (v4),
    // wm_capabilities (v5). Every request slot up to here is implemented.
    auto global = create_wl_global<&xdg_wm_base_interface,
                                   default_bind<&xdg_wm_base_interface,
                                                &wm_base_impl>>(display, 5, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return XdgShell{std::move(impl)};
}

Signal<NewToplevel>& XdgShell::new_toplevel() noexcept {
    return impl_->new_toplevel;
}

Signal<NewPopup>& XdgShell::new_popup() noexcept {
    return impl_->new_popup;
}

void XdgShell::set_bounds(int width, int height) noexcept {
    impl_->bounds_width = width;
    impl_->bounds_height = height;
}

void XdgShell::set_popup_constraint_query(PopupConstraintQuery query) {
    impl_->constraint_query = std::move(query);
}

Toplevel* toplevel_from_resource(wl_resource* resource) {
    if (resource == nullptr ||
        !wl_resource_instance_of(resource, &xdg_toplevel_interface, &toplevel_impl)) {
        return nullptr;
    }
    return static_cast<Toplevel*>(static_cast<ToplevelImpl*>(wl_resource_get_user_data(resource)));
}

Popup* popup_from_resource(wl_resource* resource) {
    if (resource == nullptr ||
        !wl_resource_instance_of(resource, &xdg_popup_interface, &popup_impl)) {
        return nullptr;
    }
    return static_cast<Popup*>(static_cast<PopupImpl*>(wl_resource_get_user_data(resource)));
}

} // namespace luminaria
