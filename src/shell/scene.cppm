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
import :pixel;

export namespace luminaria {

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

} // namespace luminaria
