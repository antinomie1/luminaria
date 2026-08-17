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

#include "detail/wayland_fwd.h"
#include <cstdint>

#include <wayland-server-core.h>
#include "wlr-layer-shell-unstable-v1-protocol.h"

export module luminaria:layer_shell;

import std;

import :box;
import :compositor;
import :display;
import :expected;
import :protocol_helper;
import :signal;
import :xdg_shell;

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

// --------------------------------------------------------------- implementation
// Implements zwlr_layer_shell_v1 / zwlr_layer_surface_v1 (version 5).
//
// The layer surface's own state is double-buffered exactly like wl_surface's:
// requests fill `pending`, wl_surface.commit promotes it to `current` and fires
// state_change so the compositor can re-run its layout. Mapping follows
// xdg-shell's rule — initial commit without a buffer, configure, then a buffer.

namespace luminaria {

struct LayerShell::Impl {
    WlGlobal global;
    wl_display* display = nullptr;
    Signal<NewLayerSurface> new_layer_surface;
};

namespace {

constexpr std::uint32_t kAllAnchors = layer_anchor_top | layer_anchor_bottom | layer_anchor_left |
                                      layer_anchor_right;

constexpr std::uint32_t opposite_edge(std::uint32_t edge) {
    switch (edge) {
    case layer_anchor_top:
        return layer_anchor_bottom;
    case layer_anchor_bottom:
        return layer_anchor_top;
    case layer_anchor_left:
        return layer_anchor_right;
    default:
        return layer_anchor_left;
    }
}

class LayerSurfaceImpl final : public LayerSurface {
public:
    struct State {
        Layer layer = Layer::background;
        std::uint32_t anchor = 0;
        int desired_w = 0, desired_h = 0;
        int exclusive_zone = 0;
        std::uint32_t exclusive_edge = 0;
        int margin_top = 0, margin_right = 0, margin_bottom = 0, margin_left = 0;
        LayerKeyboardInteractivity kb = LayerKeyboardInteractivity::none;

        [[nodiscard]] bool operator==(const State&) const = default;
    };

    LayerShell::Impl* shell = nullptr;
    Surface* surf = nullptr;
    wl_resource* resource = nullptr;
    wl_resource* output = nullptr; // the client's wl_output, or null
    std::string scope_;
    State current_, pending_;
    Signal<SurfaceCommit>::Connection commit_conn;
    Signal<SurfaceDestroy>::Connection surface_destroy_conn;
    bool configured_ = false; // a configure has been sent since (re)initialisation
    bool acked_ = false;
    bool mapped_ = false;
    bool closed_ = false;
    int pending_w_ = 0, pending_h_ = 0;
    std::uint32_t last_serial = 0;

    Surface& surface() noexcept override { return *surf; }
    [[nodiscard]] wl_resource* output_resource() const noexcept override { return output; }
    [[nodiscard]] const std::string& scope() const noexcept override { return scope_; }
    [[nodiscard]] Layer layer() const noexcept override { return current_.layer; }
    [[nodiscard]] std::uint32_t anchor() const noexcept override { return current_.anchor; }
    [[nodiscard]] int desired_width() const noexcept override { return current_.desired_w; }
    [[nodiscard]] int desired_height() const noexcept override { return current_.desired_h; }
    [[nodiscard]] int exclusive_zone() const noexcept override { return current_.exclusive_zone; }
    [[nodiscard]] std::uint32_t exclusive_edge() const noexcept override {
        return current_.exclusive_edge;
    }
    [[nodiscard]] int margin_top() const noexcept override { return current_.margin_top; }
    [[nodiscard]] int margin_right() const noexcept override { return current_.margin_right; }
    [[nodiscard]] int margin_bottom() const noexcept override { return current_.margin_bottom; }
    [[nodiscard]] int margin_left() const noexcept override { return current_.margin_left; }
    [[nodiscard]] LayerKeyboardInteractivity keyboard_interactivity() const noexcept override {
        return current_.kb;
    }
    [[nodiscard]] bool mapped() const noexcept override { return mapped_; }
    [[nodiscard]] int pending_width() const noexcept override { return pending_w_; }
    [[nodiscard]] int pending_height() const noexcept override { return pending_h_; }

    std::uint32_t configure(int width, int height) override {
        if (closed_ || resource == nullptr) {
            return 0;
        }
        // A panel that is re-configured at the size it already has repaints for
        // nothing, and layout runs on every commit — so suppress the no-op.
        if (configured_ && width == pending_w_ && height == pending_h_) {
            return last_serial;
        }
        pending_w_ = width;
        pending_h_ = height;
        configured_ = true;
        last_serial = wl_display_next_serial(shell->display);
        zwlr_layer_surface_v1_send_configure(resource, last_serial,
                                             static_cast<std::uint32_t>(std::max(0, width)),
                                             static_cast<std::uint32_t>(std::max(0, height)));
        return last_serial;
    }

    void close() override {
        if (closed_ || resource == nullptr) {
            return;
        }
        closed_ = true;
        if (mapped_) {
            mapped_ = false;
            LayerSurfaceUnmap event{*this};
            unmap.emit(event);
        }
        zwlr_layer_surface_v1_send_closed(resource);
    }

    /// The protocol's own validity rules, checked where the client can be told:
    /// on commit, against the state it just committed. Returns false after
    /// posting an error, in which case the client is already being torn down.
    [[nodiscard]] bool validate() {
        const std::uint32_t a = pending_.anchor;
        if (pending_.desired_w == 0 &&
            (a & (layer_anchor_left | layer_anchor_right)) !=
                (layer_anchor_left | layer_anchor_right)) {
            wl_resource_post_error(resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE,
                                   "width 0 requires anchoring to both left and right");
            return false;
        }
        if (pending_.desired_h == 0 &&
            (a & (layer_anchor_top | layer_anchor_bottom)) !=
                (layer_anchor_top | layer_anchor_bottom)) {
            wl_resource_post_error(resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE,
                                   "height 0 requires anchoring to both top and bottom");
            return false;
        }
        if (pending_.exclusive_edge != 0 && (pending_.exclusive_edge & a) == 0) {
            wl_resource_post_error(resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE,
                                   "exclusive edge is not an anchored edge");
            return false;
        }
        return true;
    }

    void on_commit() {
        if (closed_ || surf == nullptr) {
            return;
        }
        if (!validate()) {
            return;
        }
        const bool state_dirty = pending_ != current_;
        current_ = pending_;

        if (!configured_) {
            if (surf->has_buffer()) {
                wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED,
                                       "buffer attached before the first configure");
                return;
            }
            // Give the compositor its chance to lay us out; if it doesn't, the
            // client still has to hear something or it will never draw.
            LayerSurfaceStateChange event{*this};
            state_change.emit(event);
            if (!configured_) {
                (void)configure(current_.desired_w, current_.desired_h);
            }
            return;
        }

        if (state_dirty) {
            LayerSurfaceStateChange event{*this};
            state_change.emit(event);
        }
        if (surf->has_buffer() && !mapped_) {
            mapped_ = true;
            LayerSurfaceMap event{*this};
            map.emit(event);
        } else if (!surf->has_buffer() && mapped_) {
            mapped_ = false;
            LayerSurfaceUnmap event{*this};
            unmap.emit(event);
            // "The layer_surface returns to the state it had right after
            // get_layer_surface": it must be configured again before it re-maps.
            configured_ = false;
            acked_ = false;
            pending_w_ = pending_h_ = 0;
        }
    }
};

LayerSurfaceImpl* layer_surface_of(wl_resource* resource) {
    return static_cast<LayerSurfaceImpl*>(wl_resource_get_user_data(resource));
}

// ---- zwlr_layer_surface_v1 requests ----

void ls_set_size(wl_client*, wl_resource* resource, uint32_t width, uint32_t height) {
    LayerSurfaceImpl* ls = layer_surface_of(resource);
    ls->pending_.desired_w = static_cast<int>(width);
    ls->pending_.desired_h = static_cast<int>(height);
}
void ls_set_anchor(wl_client*, wl_resource* resource, uint32_t anchor) {
    if ((anchor & ~kAllAnchors) != 0) {
        wl_resource_post_error(resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR,
                               "invalid anchor bits");
        return;
    }
    layer_surface_of(resource)->pending_.anchor = anchor;
}
void ls_set_exclusive_zone(wl_client*, wl_resource* resource, int32_t zone) {
    layer_surface_of(resource)->pending_.exclusive_zone = zone;
}
void ls_set_margin(wl_client*, wl_resource* resource, int32_t top, int32_t right, int32_t bottom,
                   int32_t left) {
    LayerSurfaceImpl* ls = layer_surface_of(resource);
    ls->pending_.margin_top = top;
    ls->pending_.margin_right = right;
    ls->pending_.margin_bottom = bottom;
    ls->pending_.margin_left = left;
}
void ls_set_keyboard_interactivity(wl_client*, wl_resource* resource, uint32_t interactivity) {
    if (interactivity > static_cast<uint32_t>(LayerKeyboardInteractivity::on_demand)) {
        wl_resource_post_error(resource,
                               ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY,
                               "unknown keyboard interactivity mode");
        return;
    }
    layer_surface_of(resource)->pending_.kb =
        static_cast<LayerKeyboardInteractivity>(interactivity);
}
// A panel's own menu: an xdg_popup created with a null parent, anchored to us.
// The shell has no way to express that relationship, so we hand the popup to
// the compositor and let it position the popup against this surface's box.
void ls_get_popup(wl_client*, wl_resource* resource, wl_resource* popup_resource) {
    LayerSurfaceImpl* ls = layer_surface_of(resource);
    if (Popup* popup = popup_from_resource(popup_resource); popup != nullptr) {
        LayerSurfacePopup event{*ls, *popup};
        ls->new_popup.emit(event);
    }
}
void ls_ack_configure(wl_client*, wl_resource* resource, uint32_t serial) {
    LayerSurfaceImpl* ls = layer_surface_of(resource);
    if (serial == ls->last_serial) {
        ls->acked_ = true;
    }
}
void ls_set_layer(wl_client*, wl_resource* resource, uint32_t layer) {
    if (layer > static_cast<uint32_t>(Layer::overlay)) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                               "unknown layer");
        return;
    }
    layer_surface_of(resource)->pending_.layer = static_cast<Layer>(layer);
}
void ls_set_exclusive_edge(wl_client*, wl_resource* resource, uint32_t edge) {
    if (edge != 0 && (edge & (edge - 1)) != 0) {
        wl_resource_post_error(resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE,
                               "exclusive edge must name a single edge");
        return;
    }
    layer_surface_of(resource)->pending_.exclusive_edge = edge;
}

constexpr struct zwlr_layer_surface_v1_interface layer_surface_impl = {
    .set_size = ls_set_size,
    .set_anchor = ls_set_anchor,
    .set_exclusive_zone = ls_set_exclusive_zone,
    .set_margin = ls_set_margin,
    .set_keyboard_interactivity = ls_set_keyboard_interactivity,
    .get_popup = ls_get_popup,
    .ack_configure = ls_ack_configure,
    .destroy = resource_destroy_request,
    .set_layer = ls_set_layer,
    .set_exclusive_edge = ls_set_exclusive_edge,
};

void layer_surface_resource_destroy(wl_resource* resource) {
    LayerSurfaceImpl* ls = layer_surface_of(resource);
    if (ls->mapped_) {
        ls->mapped_ = false;
        LayerSurfaceUnmap unmap_event{*ls};
        ls->unmap.emit(unmap_event);
    }
    LayerSurfaceDestroy event{*ls};
    ls->destroy.emit(event);
    delete ls;
}

// ---- zwlr_layer_shell_v1 requests ----

void shell_get_layer_surface(wl_client* client, wl_resource* shell_resource, uint32_t id,
                             wl_resource* surface_resource, wl_resource* output_resource,
                             uint32_t layer, const char* scope) {
    auto* shell = static_cast<LayerShell::Impl*>(wl_resource_get_user_data(shell_resource));
    Surface* surface = surface_from_resource(surface_resource);
    if (surface == nullptr) {
        wl_resource_post_error(shell_resource, ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
                               "not a wl_surface");
        return;
    }
    if (layer > static_cast<uint32_t>(Layer::overlay)) {
        wl_resource_post_error(shell_resource, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                               "unknown layer");
        return;
    }
    if (surface->has_buffer()) {
        wl_resource_post_error(shell_resource, ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED,
                               "wl_surface already has a buffer");
        return;
    }

    auto* ls = new LayerSurfaceImpl();
    ls->shell = shell;
    ls->surf = surface;
    ls->output = output_resource;
    ls->scope_ = scope != nullptr ? scope : "";
    ls->current_.layer = ls->pending_.layer = static_cast<Layer>(layer);
    ls->resource = wl_resource_create(client, &zwlr_layer_surface_v1_interface,
                                      wl_resource_get_version(shell_resource), id);
    if (ls->resource == nullptr) {
        delete ls;
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(ls->resource, &layer_surface_impl, ls,
                                   layer_surface_resource_destroy);
    ls->commit_conn = surface->commit.connect([ls](SurfaceCommit&) { ls->on_commit(); });
    // The wl_surface can die first; after that there is nothing left to drive.
    ls->surface_destroy_conn = surface->destroy.connect([ls](SurfaceDestroy&) {
        if (ls->mapped_) {
            ls->mapped_ = false;
            LayerSurfaceUnmap event{*ls};
            ls->unmap.emit(event);
        }
        ls->surf = nullptr;
        ls->commit_conn.disconnect();
    });

    NewLayerSurface event{*ls};
    shell->new_layer_surface.emit(event);
}

constexpr struct zwlr_layer_shell_v1_interface shell_impl = {
    .get_layer_surface = shell_get_layer_surface,
    .destroy = resource_destroy_request,
};

/// Which edge an exclusive zone applies to: the one the client named, or the
/// one its anchors imply — a single edge, or an edge plus both perpendicular
/// ones (a panel spanning the top of the screen). Anything else is ambiguous
/// and reserves nothing, as the protocol says.
std::uint32_t deduced_exclusive_edge(std::uint32_t anchor, std::uint32_t explicit_edge) {
    if (explicit_edge != 0) {
        return explicit_edge;
    }
    for (std::uint32_t edge : {layer_anchor_top, layer_anchor_bottom, layer_anchor_left,
                               layer_anchor_right}) {
        const std::uint32_t triplet = kAllAnchors & ~opposite_edge(edge);
        if (anchor == edge || anchor == triplet) {
            return edge;
        }
    }
    return 0;
}

} // namespace

LayerShell::LayerShell(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LayerShell::~LayerShell() = default;
LayerShell::LayerShell(LayerShell&&) noexcept = default;
LayerShell& LayerShell::operator=(LayerShell&&) noexcept = default;

Result<LayerShell> LayerShell::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    // Version 5: set_layer (v2), the destroy request (v3), on_demand keyboard
    // interactivity (v4), set_exclusive_edge (v5). Every slot is implemented.
    auto global = create_wl_global<&zwlr_layer_shell_v1_interface,
                                   default_bind<&zwlr_layer_shell_v1_interface,
                                                &shell_impl>>(display, 5, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return LayerShell{std::move(impl)};
}

Signal<NewLayerSurface>& LayerShell::new_layer_surface() noexcept {
    return impl_->new_layer_surface;
}

Box arrange_layer_surface(LayerSurface& ls, const Box& full, Box& usable) {
    // A surface with exclusive_zone == -1 (a wallpaper, a lock screen) ignores
    // everyone else's reservations and lays itself out on the whole output.
    const Box bounds = ls.exclusive_zone() == -1 ? full : usable;
    const std::uint32_t anchor = ls.anchor();
    const bool both_h =
        (anchor & (layer_anchor_left | layer_anchor_right)) ==
        (layer_anchor_left | layer_anchor_right);
    const bool both_v =
        (anchor & (layer_anchor_top | layer_anchor_bottom)) ==
        (layer_anchor_top | layer_anchor_bottom);

    Box box{0, 0, ls.desired_width(), ls.desired_height()};

    if (box.width == 0) {
        box.x = bounds.x + ls.margin_left();
        box.width = bounds.width - (ls.margin_left() + ls.margin_right());
    } else if (both_h || (anchor & (layer_anchor_left | layer_anchor_right)) == 0) {
        box.x = bounds.x + (bounds.width - box.width) / 2;
    } else if ((anchor & layer_anchor_left) != 0) {
        box.x = bounds.x + ls.margin_left();
    } else {
        box.x = bounds.x + bounds.width - box.width - ls.margin_right();
    }

    if (box.height == 0) {
        box.y = bounds.y + ls.margin_top();
        box.height = bounds.height - (ls.margin_top() + ls.margin_bottom());
    } else if (both_v || (anchor & (layer_anchor_top | layer_anchor_bottom)) == 0) {
        box.y = bounds.y + (bounds.height - box.height) / 2;
    } else if ((anchor & layer_anchor_top) != 0) {
        box.y = bounds.y + ls.margin_top();
    } else {
        box.y = bounds.y + bounds.height - box.height - ls.margin_bottom();
    }

    box.width = std::max(0, box.width);
    box.height = std::max(0, box.height);

    if (ls.exclusive_zone() > 0) {
        const std::uint32_t edge = deduced_exclusive_edge(anchor, ls.exclusive_edge());
        switch (edge) {
        case layer_anchor_top: {
            const int taken = ls.exclusive_zone() + ls.margin_top();
            usable.y += taken;
            usable.height -= taken;
            break;
        }
        case layer_anchor_bottom:
            usable.height -= ls.exclusive_zone() + ls.margin_bottom();
            break;
        case layer_anchor_left: {
            const int taken = ls.exclusive_zone() + ls.margin_left();
            usable.x += taken;
            usable.width -= taken;
            break;
        }
        case layer_anchor_right:
            usable.width -= ls.exclusive_zone() + ls.margin_right();
            break;
        default:
            break; // ambiguous anchors reserve nothing
        }
        usable.width = std::max(0, usable.width);
        usable.height = std::max(0, usable.height);
    }

    (void)ls.configure(box.width, box.height);
    return box;
}

} // namespace luminaria
