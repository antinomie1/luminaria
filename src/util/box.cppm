// luminaria/util/box.cppm — value-semantic integer rectangle.
//
// Representative of the util layer: constexpr, no virtuals, inlineable, on the
// hot path (hit-testing, damage). More util types (region, matrix, drm_format)
// join this header set as Phase 1 needs them.

module;

export module luminaria:box;

import std;
export namespace luminaria {

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

    /// The smallest box covering both. An empty operand is ignored, so folding
    /// a list starting from `Box{}` gives the bounding box of the non-empty ones.
    [[nodiscard]] constexpr Box union_with(const Box& o) const noexcept {
        if (empty()) {
            return o;
        }
        if (o.empty()) {
            return *this;
        }
        const int x0 = std::min(x, o.x);
        const int y0 = std::min(y, o.y);
        const int x1 = std::max(x + width, o.x + o.width);
        const int y1 = std::max(y + height, o.y + o.height);
        return Box{x0, y0, x1 - x0, y1 - y0};
    }

    // Member rather than hidden friend: gcc 16 ICEs on a defaulted friend
    // operator== inside a module interface unit.
    [[nodiscard]] constexpr bool operator==(const Box&) const noexcept = default;
};

} // namespace luminaria
