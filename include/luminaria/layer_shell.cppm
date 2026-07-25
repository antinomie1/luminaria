// luminaria/layer_shell.cppm — zwlr_layer_shell_v1: the desktop's own surfaces.
//
// Panels, status bars, wallpapers, notification popups, on-screen keyboards and
// lock screens are not application windows: they belong to a *layer* of the
// output (background / bottom / top / overlay), are anchored to its edges, and
// may reserve an exclusive strip that maximized windows must not cover.
//
// The client sets layer / anchor / size / margin / exclusive zone; all of it is
// double-buffered and takes effect on the next wl_surface.commit, at which point
// `state_change` fires. Like xdg-shell, the surface maps only after the initial
// (bufferless) commit has been answered with a configure.
//
// `arrange_layer_surface()` is the geometry: give it the output box and a usable
// box, and it places the surface, shrinks the usable area by the exclusive zone,
// and configures the client. Every compositor needs exactly that arithmetic, and
// getting the corner cases wrong is how panels end up overlapping.

module;

#include "luminaria/detail/wayland_fwd.h"

#include <cstdint>
#include <memory>
#include <string>

export module luminaria:layer_shell;

import :core.expected;
import :core.signal;
import :util.box;

export namespace luminaria {

class Display;
class Popup;
class Surface;
class LayerSurface;

/// z-depth bands, bottom-most first. Application windows live between `bottom`
/// and `top`; a lock screen belongs in `overlay`.
enum class Layer : std::uint32_t {
    background = 0,
    bottom = 1,
    top = 2,
    overlay = 3,
};

/// Edges of the output the surface sticks to (a bitmask; values match
/// `zwlr_layer_surface_v1.anchor`).
enum LayerAnchor : std::uint32_t {
    layer_anchor_top = 1,
    layer_anchor_bottom = 2,
    layer_anchor_left = 4,
    layer_anchor_right = 8,
};

/// Whether the surface may hold keyboard focus. `exclusive` is what a lock
/// screen or a password prompt asks for; `on_demand` is a panel that can be
/// clicked into; `none` (the default) never takes focus.
enum class LayerKeyboardInteractivity : std::uint32_t {
    none = 0,
    exclusive = 1,
    on_demand = 2,
};

struct NewLayerSurface {
    LayerSurface& layer_surface;
};
struct LayerSurfaceMap {
    LayerSurface& layer_surface;
};
struct LayerSurfaceUnmap {
    LayerSurface& layer_surface;
};
struct LayerSurfaceDestroy {
    LayerSurface& layer_surface;
};
/// The client changed its layer / anchor / size / margin / exclusive zone (all
/// of which are double-buffered, so this fires on commit). Re-run the layout.
struct LayerSurfaceStateChange {
    LayerSurface& layer_surface;
};
/// A panel's own menu: an xdg_popup the client anchored to this layer surface
/// rather than to another xdg_surface. `Popup::parent_surface()` is null for
/// such a popup — this signal is the only way to learn what it belongs to, so
/// position it against wherever this layer surface was placed.
struct LayerSurfacePopup {
    LayerSurface& layer_surface;
    Popup& popup;
};

/// One zwlr_layer_surface_v1. Owned by its resource; address stable for its
/// lifetime, so signals may capture `LayerSurface&`.
class LayerSurface {
public:
    virtual ~LayerSurface() = default;
    LayerSurface(const LayerSurface&) = delete;
    LayerSurface& operator=(const LayerSurface&) = delete;

    Signal<LayerSurfaceMap> map;
    Signal<LayerSurfaceUnmap> unmap;
    Signal<LayerSurfaceDestroy> destroy;
    Signal<LayerSurfaceStateChange> state_change;
    Signal<LayerSurfacePopup> new_popup;

    [[nodiscard]] virtual Surface& surface() noexcept = 0;
    /// The wl_output the client asked for, or null for "compositor's choice"
    /// (which in practice means the output the user last used).
    [[nodiscard]] virtual wl_resource* output_resource() const noexcept = 0;
    /// The client's stated purpose ("panel", "notifications", …). Called
    /// `namespace` in the protocol, which is a keyword here.
    [[nodiscard]] virtual const std::string& scope() const noexcept = 0;

    [[nodiscard]] virtual Layer layer() const noexcept = 0;
    /// Bitmask of LayerAnchor.
    [[nodiscard]] virtual std::uint32_t anchor() const noexcept = 0;
    /// Size the client asked for, in surface coordinates. 0 on an axis means
    /// "compositor decides", and is only legal when anchored to both of that
    /// axis' edges.
    [[nodiscard]] virtual int desired_width() const noexcept = 0;
    [[nodiscard]] virtual int desired_height() const noexcept = 0;
    /// Strip along the anchored edge that other windows must not cover.
    /// 0 = "no strip, but move me out of everyone else's"; -1 = "ignore other
    /// surfaces' zones and stretch me to the edges I am anchored to".
    [[nodiscard]] virtual int exclusive_zone() const noexcept = 0;
    /// Which edge the exclusive zone applies to when the anchors alone are
    /// ambiguous (a corner). 0 = deduce it. A single LayerAnchor bit.
    [[nodiscard]] virtual std::uint32_t exclusive_edge() const noexcept = 0;
    [[nodiscard]] virtual int margin_top() const noexcept = 0;
    [[nodiscard]] virtual int margin_right() const noexcept = 0;
    [[nodiscard]] virtual int margin_bottom() const noexcept = 0;
    [[nodiscard]] virtual int margin_left() const noexcept = 0;
    [[nodiscard]] virtual LayerKeyboardInteractivity keyboard_interactivity() const noexcept = 0;

    [[nodiscard]] virtual bool mapped() const noexcept = 0;
    /// Size we last asked the client to take (0 before the first configure).
    [[nodiscard]] virtual int pending_width() const noexcept = 0;
    [[nodiscard]] virtual int pending_height() const noexcept = 0;

    /// Ask the client to take this size in surface coordinates (0 = "you
    /// choose"). Returns the configure serial. Sending the same size twice is
    /// suppressed: a panel that gets configured every frame repaints every frame.
    virtual std::uint32_t configure(int width, int height) = 0;
    /// Tell the client the surface is gone for good (its output vanished, or the
    /// user dismissed it). It is expected to destroy the object; nothing it does
    /// afterwards has any effect.
    virtual void close() = 0;

protected:
    LayerSurface() = default;
};

/// The zwlr_layer_shell_v1 global (version 5). Move-only; pointer-stable state.
class LayerShell {
public:
    [[nodiscard]] static Result<LayerShell> create(Display& display);

    ~LayerShell();
    LayerShell(LayerShell&&) noexcept;
    LayerShell& operator=(LayerShell&&) noexcept;
    LayerShell(const LayerShell&) = delete;
    LayerShell& operator=(const LayerShell&) = delete;

    /// Fires when a client gives a wl_surface the layer-surface role. Nothing is
    /// on screen yet — the surface maps later, after its first configure.
    [[nodiscard]] Signal<NewLayerSurface>& new_layer_surface() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit LayerShell(std::unique_ptr<Impl> impl) noexcept;
};

/// Lay `ls` out on an output and configure it. `full` is the whole output in
/// layout coordinates; `usable` is what previous layer surfaces have left of it
/// and is SHRUNK here by this surface's exclusive zone. Returns the box the
/// surface should be drawn at.
///
/// Call it for every layer surface on an output, in ascending layer order and
/// exclusive-zone-first within a layer, then hand the final `usable` to
/// XdgShell::set_bounds() — that is how a maximized window learns to stop above
/// the panel.
[[nodiscard]] Box arrange_layer_surface(LayerSurface& ls, const Box& full, Box& usable);

} // namespace luminaria
