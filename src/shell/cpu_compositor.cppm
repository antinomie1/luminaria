// luminaria/shell/cpu_compositor.cppm — generic CPU compositor for surface trees.
//
// The GPU shell (`Frame`) needs a Vulkan device; this is the whole composite
// half for a compositor that has none. It paints a z-ordered list of
// compositor-owned primitives — client surface trees and solid rectangles —
// onto a tightly-packed RGBA buffer with premultiplied source-over blending.
//
//     CpuCompositor cpu;
//     std::vector<CpuItem> items;
//     items.emplace_back(RectFill{{0, 0, 800, 600}, kWallpaper});
//     for (const Window& w : windows) items.emplace_back(CpuView{w.id, w.x, w.y});
//     cpu.composite(800, 600, kBackground, items);
//     output.commit_frame(cpu.pixels(), cpu.width(), cpu.height());
//
// The draw list is immediate-mode like everything in the shell layer: build it
// per frame, hand it over, forget it. Identity is generational (`SurfaceId`),
// so a list that accidentally survives a dispatch resolves to nothing instead
// of a recycled surface. A surface whose buffer is not readable (a dmabuf, an
// unsupported format) is skipped; the rest of the frame still draws.

module;

#include <cstdint>

export module luminaria:cpu_compositor;

import std;

import :box;
import :color;
import :compositor;
import :pixel;
import :rect_fill;
import :transform;

export namespace luminaria {

/// One client surface tree in the draw list: the root at (x,y), which drags
/// its whole subsurface tree along. Resolved at draw time, so the tree is
/// read as it stands now.
///
/// `clip` is a device-space box every surface of the tree is confined to — a
/// tiling compositor clips each window to its tile so an old-size surface
/// never paints over its neighbour. Empty means "no clip": the framebuffer
/// bounds are the only limit, which is what the whole-output case (a cursor
/// tree, a maximized window) wants.
struct CpuView {
    SurfaceId surface;
    int x = 0;
    int y = 0;
    Box clip;
};

/// One z-ordered draw item: a solid rectangle the compositor owns, or a client
/// surface tree. List order is draw order, back-to-front.
using CpuItem = std::variant<RectFill, CpuView>;

/// Generic CPU compositor. Holds its own scratch, so a steady-state frame
/// allocates nothing beyond the first.
class CpuCompositor {
public:
    /// Paint a width×height frame: clear to `background`, then draw `items`
    /// back-to-front. `pixels()` is valid until the next call.
    void composite(int width, int height, Color background, std::span<const CpuItem> items);

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

    /// The finished frame, tightly-packed RGBA8 pixels.
    [[nodiscard]] std::span<const Pixel> pixels() const noexcept { return pixels_; }

private:
    void fill_rect(const RectFill& fill);
    void draw_view(const CpuView& view);
    void draw_surface(Surface& surface, int x, int y, const Box& clip);

    int width_ = 0;
    int height_ = 0;
    std::vector<Pixel> pixels_;
    std::vector<std::uint8_t> rgba_; // per-surface buffer read-back
    std::vector<SurfaceAt> tree_;    // per-tree walk
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

namespace {

/// Color -> straight-alpha RGBA8 pixel.
[[nodiscard]] Pixel to_pixel(Color c) noexcept {
    const auto byte = [](float v) {
        return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return Pixel{byte(c.r), byte(c.g), byte(c.b), byte(c.a)};
}

/// Premultiplied source-over, the way wl_shm buffers (ARGB8888) and the GPU
/// path carry alpha: `dst = src + dst * (1 - src.a)`. Exact for the normal
/// case of an opaque backdrop, which is also what XRGB8888 scanout assumes.
void blend(Pixel& dst, Pixel src) noexcept {
    const unsigned inv = 255u - src.a;
    if (inv == 0u) {
        dst = src;
        return;
    }
    dst.r = static_cast<std::uint8_t>(src.r + dst.r * inv / 255u);
    dst.g = static_cast<std::uint8_t>(src.g + dst.g * inv / 255u);
    dst.b = static_cast<std::uint8_t>(src.b + dst.b * inv / 255u);
    dst.a = static_cast<std::uint8_t>(src.a + dst.a * inv / 255u);
}

/// The per-pixel point mapping of `transform` (buffer -> surface) on the unit
/// square: the inverse of what `Surface::buffer_source_uv` does to the crop
/// rect, so sampling the raw buffer at the result lands on the pixel the
/// shader's texture fetch would.
void unit_point(Transform t, double& x, double& y) noexcept {
    if (transform_flipped(t)) {
        x = 1.0 - x;
    }
    switch (transform_rotation(t)) {
    case 90: {
        const double nx = 1.0 - y;
        y = x;
        x = nx;
        break;
    }
    case 180:
        x = 1.0 - x;
        y = 1.0 - y;
        break;
    case 270: {
        const double nx = y;
        y = 1.0 - x;
        x = nx;
        break;
    }
    default:
        break;
    }
}

} // namespace

void CpuCompositor::composite(int width, int height, Color background,
                              std::span<const CpuItem> items) {
    width_ = width;
    height_ = height;
    pixels_.assign(static_cast<std::size_t>(width) * height, to_pixel(background));
    for (const CpuItem& item : items) {
        if (const auto* fill = std::get_if<RectFill>(&item); fill != nullptr) {
            fill_rect(*fill);
        } else if (const auto* view = std::get_if<CpuView>(&item); view != nullptr) {
            draw_view(*view);
        }
    }
}

void CpuCompositor::fill_rect(const RectFill& fill) {
    const Box box = fill.box.intersection(Box{0, 0, width_, height_});
    if (box.empty()) {
        return;
    }
    const Pixel color = to_pixel(fill.color);
    for (int y = box.y; y < box.y + box.height; ++y) {
        for (int x = box.x; x < box.x + box.width; ++x) {
            blend(pixels_[static_cast<std::size_t>(y) * width_ + x], color);
        }
    }
}

void CpuCompositor::draw_view(const CpuView& view) {
    Surface* surface = surface_from_id(view.surface);
    if (surface == nullptr) {
        return;
    }
    // The view clip and the framebuffer bounds both apply; the tree is drawn
    // only where they overlap, so a clip that pokes off the edge is safe.
    const Box framebuffer{0, 0, width_, height_};
    const Box clip = view.clip.empty() ? framebuffer : view.clip.intersection(framebuffer);
    if (clip.empty()) {
        return;
    }
    tree_.clear();
    surface->surface_tree(tree_);
    for (const SurfaceAt& at : tree_) {
        draw_surface(*at.surface, view.x + at.x, view.y + at.y, clip);
    }
}

void CpuCompositor::draw_surface(Surface& surface, int x, int y, const Box& clip) {
    int buffer_w = 0;
    int buffer_h = 0;
    if (!surface.current_buffer_rgba(rgba_, buffer_w, buffer_h)) {
        return;
    }
    const int surface_w = surface.surface_width();
    const int surface_h = surface.surface_height();
    if (surface_w <= 0 || surface_h <= 0) {
        return;
    }
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    surface.buffer_source_uv(u0, v0, u1, v1);
    const Transform transform = surface.buffer_transform();
    // The surface's own rectangle, confined to the view clip: pixels the tree
    // would paint outside it are simply never touched, so whatever was already
    // there (a neighbour, the compositor's own rects) stays byte for byte.
    const Box box = Box{x, y, surface_w, surface_h}.intersection(clip);
    if (box.empty()) {
        return;
    }
    // Nearest-neighbour sampling: surface point -> normalized crop (u,v) ->
    // raw-buffer pixel. The scale and the viewport crop are both already
    // folded into the UVs by buffer_source_uv, exactly as for the GPU path.
    const double du = (u1 - u0) / surface_w;
    const double dv = (v1 - v0) / surface_h;
    for (int py = box.y; py < box.y + box.height; ++py) {
        const double v = v0 + (py - y + 0.5) * dv;
        for (int px = box.x; px < box.x + box.width; ++px) {
            double u = u0 + (px - x + 0.5) * du;
            double w = v;
            unit_point(transform, u, w);
            const int bx = std::clamp(static_cast<int>(u * buffer_w), 0, buffer_w - 1);
            const int by = std::clamp(static_cast<int>(w * buffer_h), 0, buffer_h - 1);
            const std::size_t at = (static_cast<std::size_t>(by) * buffer_w + bx) * 4;
            blend(pixels_[static_cast<std::size_t>(py) * width_ + px],
                  Pixel{rgba_[at], rgba_[at + 1], rgba_[at + 2], rgba_[at + 3]});
        }
    }
}

} // namespace luminaria
