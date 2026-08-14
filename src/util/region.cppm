// luminaria/util/region.cppm — a set of pixels as a list of disjoint boxes.
//
// Wayland hands us regions everywhere: wl_region for input and opaque areas,
// wl_surface.damage for what changed. Both need union, subtraction and a point
// test, and both are read once per frame — so this is a plain vector of
// non-overlapping boxes rather than a pixman region. Disjointness is the whole
// point: a translucent surface painted twice over the same pixel blends twice.
//
// Every mutation ends in a coalescing pass, so the box count tracks the shape of
// the region rather than the history of how it was built: a client that damages a
// window row by row leaves one box behind, not one per row. That matters because
// the renderer turns each box into its own scissored draw.
//
// ponytail: the ops are O(n) and coalescing is O(n²) on top, which is the right
// trade while n stays in the tens. Swap in pixman_region32 if a real workload ever
// makes the box count matter.

module;

#include <cstddef>
#include <vector>

export module luminaria:region;

import :box;

export namespace luminaria {

class Region {
public:
    Region() = default;
    explicit Region(const Box& box) { add(box); }

    /// The disjoint boxes covering this region. Order is unspecified.
    [[nodiscard]] const std::vector<Box>& rects() const noexcept { return rects_; }
    [[nodiscard]] bool empty() const noexcept { return rects_.empty(); }
    void clear() noexcept { rects_.clear(); }

    void add(const Box& box) {
        if (box.empty()) {
            return;
        }
        subtract(box); // keep the invariant: nothing overlaps
        rects_.push_back(box);
        coalesce();
    }

    void add(const Region& other) {
        for (const Box& b : other.rects_) {
            add(b);
        }
    }

    /// Remove `box` from the region, splitting the boxes it cuts through.
    void subtract(const Box& box) {
        if (box.empty() || rects_.empty()) {
            return;
        }
        std::vector<Box> out;
        out.reserve(rects_.size());
        for (const Box& r : rects_) {
            const Box hit = r.intersection(box);
            if (hit.empty()) {
                out.push_back(r);
                continue;
            }
            // Up to four survivors: above, below, left and right of the hole.
            if (hit.y > r.y) {
                out.push_back(Box{r.x, r.y, r.width, hit.y - r.y});
            }
            if (hit.y + hit.height < r.y + r.height) {
                out.push_back(Box{r.x, hit.y + hit.height, r.width,
                                  (r.y + r.height) - (hit.y + hit.height)});
            }
            if (hit.x > r.x) {
                out.push_back(Box{r.x, hit.y, hit.x - r.x, hit.height});
            }
            if (hit.x + hit.width < r.x + r.width) {
                out.push_back(Box{hit.x + hit.width, hit.y, (r.x + r.width) - (hit.x + hit.width),
                                  hit.height});
            }
        }
        rects_ = std::move(out);
        coalesce();
    }

    void subtract(const Region& other) {
        for (const Box& b : other.rects_) {
            subtract(b);
        }
    }

    /// Drop everything outside `box`.
    void intersect(const Box& box) {
        std::vector<Box> out;
        out.reserve(rects_.size());
        for (const Box& r : rects_) {
            if (const Box hit = r.intersection(box); !hit.empty()) {
                out.push_back(hit);
            }
        }
        rects_ = std::move(out);
        coalesce();
    }

    void translate(int dx, int dy) noexcept {
        for (Box& r : rects_) {
            r.x += dx;
            r.y += dy;
        }
    }

    [[nodiscard]] bool contains(int px, int py) const noexcept {
        for (const Box& r : rects_) {
            if (r.contains(px, py)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool intersects(const Box& box) const noexcept {
        for (const Box& r : rects_) {
            if (!r.intersection(box).empty()) {
                return true;
            }
        }
        return false;
    }

    /// The smallest box covering the whole region (empty if the region is).
    [[nodiscard]] Box extents() const noexcept {
        Box e{};
        for (const Box& r : rects_) {
            e = e.union_with(r);
        }
        return e;
    }

    // Member rather than hidden friend: gcc 16 ICEs on a defaulted friend
    // operator== inside a module interface unit.
    [[nodiscard]] bool operator==(const Region&) const = default;

private:
    /// Merge every pair of boxes that shares a full edge, until none is left.
    ///
    /// Disjointness is what makes this sound: two boxes that agree exactly on one
    /// axis and touch on the other tile a rectangle with no gap and no overlap, so
    /// their bounding box *is* their union. Boxes that merely overlap or abut
    /// partially are left alone — merging those would claim pixels the region does
    /// not contain.
    void coalesce() noexcept {
        for (bool merged = true; merged;) {
            merged = false;
            for (std::size_t i = 0; i < rects_.size() && !merged; ++i) {
                for (std::size_t j = i + 1; j < rects_.size(); ++j) {
                    const Box& a = rects_[i];
                    const Box& b = rects_[j];
                    const bool stacked = a.x == b.x && a.width == b.width &&
                                         (a.y + a.height == b.y || b.y + b.height == a.y);
                    const bool side_by_side = a.y == b.y && a.height == b.height &&
                                              (a.x + a.width == b.x || b.x + b.width == a.x);
                    if (!stacked && !side_by_side) {
                        continue;
                    }
                    rects_[i] = a.union_with(b);
                    rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(j));
                    merged = true;
                    break;
                }
            }
        }
    }

    std::vector<Box> rects_; // pairwise disjoint, and maximally coalesced
};

} // namespace luminaria
