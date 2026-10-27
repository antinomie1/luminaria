// luminaria/shell/scene.cppm — one ordered list, drawn and hit-tested.
//
// `Frame` and `CpuCompositor` are the two ways pixels reach a display, and a
// compositor that has to feed both ends up writing every primitive twice — once
// as a `Placement`, once as a `CpuItem` — with no compiler to notice when the
// two drift. `SceneItem` is the single list both are built from
// (`SceneRenderer`, in `luminaria.gpu`), and the same list this file hit-tests.
//
// That is the point rather than a convenience: a compositor that answers "what
// is under the cursor" against anything other than the rectangles it drew is
// guessing, and it shows up as clicks landing on the wrong window mid-relayout.
//
// Still immediate-mode, like everything in the shell layer (ADR 0001): build the
// list per frame, hand it over, forget it. Identity is generational, so a list
// that outlives a dispatch resolves to nothing rather than to a recycled
// surface, and `tag` is the caller's own identity — luminaria never reads it,
// it only hands it back from a hit.

module;

#include <cstdint>

export module luminaria:scene;

import std;

import :box;
import :color;
import :compositor;
import :cursor_theme;
import :pixel;
import :seat;

export namespace luminaria {

/// A software cursor drawn above the scene. Visual only, and therefore never in
/// the scene list and never hit-tested.
struct SceneCursor {
    SurfaceId surface{};
    const CursorImage* image = nullptr;
    int x = 0, y = 0;
};


/// How much a blur blurs.
///
/// The names are niri's rather than Hyprland's single `size`, because the two
/// knobs do not cost the same and one number hides which one is being turned.
/// `passes` buys radius by halving the resolution again — each one is another
/// pair of render passes — while `offset` buys radius inside the passes already
/// paid for and is free. Reach for `offset` first; add a pass when the samples
/// start to separate into a visible cross.
struct BlurParams {
    /// Down/up levels. Clamped to what the chain was built for.
    int passes = 3;
    /// Tap distance, in half source texels. Above ~4 the five taps stop
    /// overlapping and the blur turns into a star.
    float offset = 2.0f;
    /// Dither amplitude on the final upsample, 0 for none. A large smooth
    /// gradient banded into 8-bit is the artefact this exists for.
    float noise = 0.0f;
    /// 1.0 leaves colour alone. Above it, undoes the averaging-towards-grey a
    /// blur necessarily does.
    float saturation = 1.0f;

    [[nodiscard]] constexpr bool operator==(const BlurParams&) const noexcept = default;
};

/// How far a blurred pixel reaches for its input, in source pixels.
///
/// Damage, not decoration: a blurred window shows what is behind it from this
/// far outside itself, so a change anywhere within this distance of it makes it
/// stale. Deliberately generous — repainting a little too much is a cost, and
/// repainting too little is a smear that stays on the screen.
[[nodiscard]] inline int blur_spread(const BlurParams& params) noexcept {
    const int passes = std::clamp(params.passes, 1, 6);
    return static_cast<int>(
        std::ceil(2.0f * std::max(params.offset, 0.0f) * static_cast<float>(1 << passes)));
}

/// Blur what is behind an item. GPU only, and off by default: it is the most
/// expensive thing in the scene list and nothing should pay for it unasked.
struct SceneBlur {
    bool enabled = false;
    /// Blur the scene *below the lowest blurred item* once and share it,
    /// instead of capturing a backdrop per item. That is what makes it cheap,
    /// and it is also exactly x-ray's defining property: a translucent window
    /// shows the wallpaper and the layers behind it, never the window below it.
    bool xray = false;
    /// For a `surface` item, blur only the regions the client declared through
    /// ext-background-effect-v1 instead of the whole item box: the committed
    /// `blur_region()` rectangles of the root and subsurfaces, translated by
    /// the item's root origin and clipped to each child's surface, the item
    /// box, the output view and the backdrop. Empty regions blur nothing and
    /// cost no capture. A non-surface item has no tree to ask, so
    /// `surface_regions` blurs nothing for one.
    bool surface_regions = false;
    BlurParams params{};

    [[nodiscard]] constexpr bool operator==(const SceneBlur&) const noexcept = default;
};

/// A drop shadow cast by an item's box, drawn under it. `color.a` of 0 is none.
/// GPU only.
struct SceneShadow {
    /// Fully transparent, and that is what "no shadow" is: `Color{}` is opaque
    /// black, so a default-constructed shadow would otherwise be a shadow.
    Color color{0.0f, 0.0f, 0.0f, 0.0f};
    /// How far it fades out beyond the box, in logical pixels.
    float feather = 0.0f;

    [[nodiscard]] constexpr bool operator==(const SceneShadow&) const noexcept = default;
};

/// One entry in the paint list. Order is paint order, back to front.
struct SceneItem {
    enum class Kind {
        /// A client surface tree, its root at (`x`, `y`), clipped to `box`.
        surface,
        /// A solid rectangle filling `box`.
        rect,
        /// A ring `thickness` thick just *inside* `box` — a window border. Drawn
        /// as four rectangles that do not overlap, so no pixel is blended twice
        /// and a translucent border stays one colour; hit-tested as the whole
        /// `box`, because the frame around a window belongs to that window.
        border,
        /// Pixels the compositor itself owns: a rasterized bar, a themed
        /// cursor. `box.width * box.height` premultiplied RGBA, tightly packed,
        /// *borrowed* until the frame is presented — whoever appended the item
        /// keeps the buffer alive at least that long.
        image,
    };

    Kind kind = Kind::surface;
    /// Logical coordinates: what is painted, and what is hit.
    Box box;
    SurfaceId surface{};              ///< `surface` only
    int x = 0, y = 0;                 ///< `surface` only: the root's origin
    Color color{};                    ///< `rect` and `border`
    int thickness = 0;                ///< `border` only
    std::span<const Pixel> pixels{};  ///< `image` only
    /// `image` only: bumped whenever those pixels change, so the GPU path can
    /// keep a texture across frames instead of comparing a megabyte to find out
    /// it did not. Two items that are the n-th image of their frame and share a
    /// serial are the same picture.
    std::uint64_t serial = 0;
    /// Whatever identity the caller wants back from `scene_hit_test`. Opaque
    /// here: luminaria never interprets it.
    std::uint64_t tag = 0;
    /// False for something drawn but never hit — a bar, a wallpaper.
    bool accepts_input = true;

    /// Whole-item opacity. On the GPU a `surface` item is composited offscreen
    /// first, so a window with subsurfaces fades as one image rather than
    /// seaming where they overlap (ADR 0005); the CPU path has no offscreen and
    /// fades each surface, which is visible only on a tree that overlaps
    /// itself.
    float alpha = 1.0f;

    /// Rounded corners, in logical pixels. GPU only — the CPU path draws square
    /// corners rather than refusing to come up, the same way a missing font
    /// leaves a label blank.
    float corner_radius = 0.0f;

    /// Blur what is behind this item, inside `box`, following `corner_radius`.
    SceneBlur blur{};

    /// A drop shadow drawn under this item. `shadow_box` may name a frame larger
    /// than `box` — for example server-side decorations around a client surface;
    /// empty means the item's own box.
    SceneShadow shadow{};
    Box shadow_box{};
};

/// What is under a point.
///
/// `surface` is resolved at the moment of the hit and must not be retained past
/// the dispatch that produced it; `tag` is the identity that may be. A hit with
/// a null `surface` is the ordinary case of a point on a border, on a solid
/// rectangle, or on a part of a surface the client asked not to receive input.
struct SceneHit {
    bool hit = false;
    std::uint64_t tag = 0;
    Surface* surface = nullptr;
    double sx = 0.0, sy = 0.0;
};

/// Front to back — the reverse of paint order — against the same list that was
/// drawn.
[[nodiscard]] SceneHit scene_hit_test(std::span<const SceneItem> scene, double x, double y);

/// A point in the scene's coordinates.
struct ScenePoint {
    double x = 0.0, y = 0.0;
};

/// Where `surface`'s own origin is drawn in `scene`: a root's item origin, or
/// that plus a subsurface's offset in its tree. Empty when it is not drawn.
[[nodiscard]] std::optional<ScenePoint> scene_surface_origin(std::span<const SceneItem> scene,
                                                             SurfaceId surface);

/// Tell the seat the pointer is at (`x`, `y`) in `scene`.
///
/// With a button held (an implicit grab, `Seat::pointer_grab()`) every motion
/// goes to the surface that took the press, in its own coordinates, wherever
/// the pointer is; otherwise the surface under the point gets focus, or focus
/// is cleared over nothing. During a drag the seat routes to the drag instead.
/// Answers the hit under the point either way, for a compositor that also
/// wants to know which window that was.
SceneHit scene_route_pointer(Seat& seat, std::span<const SceneItem> scene, double x, double y);

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

SceneHit scene_hit_test(std::span<const SceneItem> scene, double x, double y) {
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    for (const SceneItem& item : std::views::reverse(scene)) {
        if (!item.accepts_input || !item.box.contains(ix, iy)) {
            continue;
        }
        if (item.kind != SceneItem::Kind::surface) {
            return {.hit = true, .tag = item.tag};
        }
        Surface* root = surface_from_id(item.surface);
        if (root == nullptr) {
            continue;
        }
        // The client's input region decides, not its buffer rectangle — so a
        // miss falls through to whatever is behind rather than swallowing it.
        const std::optional<SurfaceAt> found = root->surface_at(x - item.x, y - item.y);
        if (found.has_value()) {
            return {.hit = true,
                    .tag = item.tag,
                    .surface = found->surface,
                    .sx = x - item.x - found->x,
                    .sy = y - item.y - found->y};
        }
    }
    return {};
}

std::optional<ScenePoint> scene_surface_origin(std::span<const SceneItem> scene,
                                               SurfaceId surface) {
    if (!surface.valid()) {
        return std::nullopt;
    }
    for (const SceneItem& item : scene) {
        if (item.kind == SceneItem::Kind::surface && item.surface == surface) {
            return ScenePoint{static_cast<double>(item.x), static_cast<double>(item.y)};
        }
    }
    for (const SceneItem& item : scene) {
        Surface* root = item.kind == SceneItem::Kind::surface ? surface_from_id(item.surface)
                                                              : nullptr;
        if (root == nullptr) {
            continue;
        }
        for (const SurfaceAt& at : root->surface_tree()) {
            if (at.surface->id() == surface) {
                return ScenePoint{static_cast<double>(item.x + at.x),
                                  static_cast<double>(item.y + at.y)};
            }
        }
    }
    return std::nullopt;
}

SceneHit scene_route_pointer(Seat& seat, std::span<const SceneItem> scene, double x, double y) {
    const SceneHit hit = scene_hit_test(scene, x, y);
    if (const SurfaceId grab = seat.pointer_grab(); grab.valid() && !seat.dragging()) {
        if (const std::optional<ScenePoint> origin = scene_surface_origin(scene, grab)) {
            seat.pointer_motion(x - origin->x, y - origin->y);
        }
        return hit;
    }
    if (hit.surface != nullptr) {
        seat.pointer_enter(*hit.surface, hit.sx, hit.sy);
    } else if (seat.pointer_focus().valid()) {
        seat.pointer_clear_focus();
    }
    return hit;
}

} // namespace luminaria
