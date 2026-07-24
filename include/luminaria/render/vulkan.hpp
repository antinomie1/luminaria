// luminaria/render/vulkan.hpp — Vulkan renderer (Vulkan-Hpp RAII under the hood).
//
// Public header leaks no Vulkan headers: all vk::raii state hides behind Impl.
// Vulkan-Hpp uses exceptions internally; they are caught at every method
// boundary and turned into Result, so nothing throws across a C callback.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "luminaria/core/expected.hpp"
#include "luminaria/util/color.hpp"
#include "luminaria/util/pixel.hpp"
#include "luminaria/util/rect_fill.hpp"

namespace luminaria {

/// A surface's pixels placed at (x,y). `rgba` is w*h*4 tightly-packed RGBA8.
struct TextureFill {
    int x, y, w, h;
    const std::uint8_t* rgba;
};

class VulkanRenderer {
public:
    /// Bring up an instance + device. Fails if no Vulkan device is available.
    [[nodiscard]] static Result<VulkanRenderer> create();

    ~VulkanRenderer();
    VulkanRenderer(VulkanRenderer&&) noexcept;
    VulkanRenderer& operator=(VulkanRenderer&&) noexcept;
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    /// Render a solid-color frame offscreen and read back pixel (0,0). Proves the
    /// GPU path end to end; real surface compositing arrives in Phase 2.
    [[nodiscard]] Result<Pixel> render_clear_readback(int width, int height, Color color);

    /// Composite: clear to `background`, then paint each RectFill (back-to-front)
    /// into a width×height frame. Returns the frame as row-major RGBA pixels.
    [[nodiscard]] Result<std::vector<Pixel>> render_rects(int width, int height, Color background,
                                                          std::span<const RectFill> rects);

    /// Composite rects, then blit surface textures (opaque, 1:1) on top at their
    /// positions. `rgba` is tightly-packed w×h RGBA8. Returns row-major RGBA.
    // TODO: copy-based placement — no scaling, no alpha blend; upgrade to a
    // textured-quad pipeline when translucency or scaled output matters.
    [[nodiscard]] Result<std::vector<Pixel>> composite(int width, int height, Color background,
                                                       std::span<const RectFill> rects,
                                                       std::span<const TextureFill> textures);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit VulkanRenderer(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
