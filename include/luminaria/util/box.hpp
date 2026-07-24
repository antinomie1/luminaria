// luminaria/util/box.hpp — value-semantic integer rectangle.
//
// Representative of the util layer: constexpr, no virtuals, inlineable, on the
// hot path (hit-testing, damage). More util types (region, matrix, drm_format)
// join this header set as Phase 1 needs them.
#pragma once

#include <algorithm>

namespace luminaria {

/// An axis-aligned rectangle in integer coordinates. Empty iff width/height <= 0.
struct Box {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr bool empty() const noexcept {
        return width <= 0 || height <= 0;
    }

    [[nodiscard]] constexpr bool contains(int px, int py) const noexcept {
        return px >= x && py >= y && px < x + width && py < y + height;
    }

    /// The overlap of two boxes, or an empty box if they are disjoint.
    [[nodiscard]] constexpr Box intersection(const Box& o) const noexcept {
        const int x0 = std::max(x, o.x);
        const int y0 = std::max(y, o.y);
        const int x1 = std::min(x + width, o.x + o.width);
        const int y1 = std::min(y + height, o.y + o.height);
        if (x1 <= x0 || y1 <= y0) {
            return Box{};
        }
        return Box{x0, y0, x1 - x0, y1 - y0};
    }

    friend constexpr bool operator==(const Box&, const Box&) = default;
};

} // namespace luminaria
