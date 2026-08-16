// Region has one invariant that matters: its boxes never overlap. A translucent
// surface painted twice over the same pixel blends twice, and damage that
// double-counts is exactly how that happens.
#include <cassert>

import luminaria;
import std;

namespace {

// Total covered area, which is only equal to the sum of the boxes when they are
// disjoint — so this doubles as the overlap check.
int area(const luminaria::Region& r) {
    return std::accumulate(r.rects().begin(), r.rects().end(), 0,
                           [](int a, const luminaria::Box& b) { return a + b.width * b.height; });
}

} // namespace

int main() {
    using luminaria::Box;
    using luminaria::Region;

    Region r;
    assert(r.empty());
    r.add(Box{0, 0, 10, 10});
    assert(area(r) == 100);

    // Overlapping adds must not double-count.
    r.add(Box{5, 5, 10, 10});
    assert(area(r) == 100 + 100 - 25);
    assert(r.contains(0, 0));
    assert(r.contains(14, 14));
    assert(!r.contains(14, 2));
    assert(r.extents() == (Box{0, 0, 15, 15}));

    // A hole punched through the middle leaves a ring, still disjoint.
    Region ring{Box{0, 0, 10, 10}};
    ring.subtract(Box{3, 3, 4, 4});
    assert(area(ring) == 100 - 16);
    assert(!ring.contains(4, 4));
    assert(ring.contains(2, 4));
    assert(ring.contains(7, 4));

    // Subtracting more than there is, and subtracting nothing.
    Region gone{Box{0, 0, 4, 4}};
    gone.subtract(Box{-5, -5, 100, 100});
    assert(gone.empty());
    Region intact{Box{0, 0, 4, 4}};
    intact.subtract(Box{100, 100, 4, 4});
    assert(area(intact) == 16);

    // Intersect clips, translate moves.
    Region clipped{Box{0, 0, 10, 10}};
    clipped.add(Box{20, 0, 10, 10});
    clipped.intersect(Box{5, 0, 20, 10});
    assert(area(clipped) == 50 + 50);
    clipped.translate(1, 2);
    assert(clipped.extents() == (Box{6, 2, 20, 10}));

    // Region-wise ops.
    Region a{Box{0, 0, 10, 10}};
    Region b{Box{5, 0, 10, 10}};
    a.subtract(b);
    assert(area(a) == 50);
    a.add(b);
    assert(area(a) == 150);

    // --- coalescing: the box count must follow the shape of the region, not the
    //     order it was built in. Each box becomes its own scissored draw, so a
    //     client damaging a window row by row must not cost one draw per row. ---

    // Rows stacked into one rectangle.
    Region rows;
    for (int y = 0; y < 20; ++y) {
        rows.add(Box{0, y, 40, 1});
    }
    assert(rows.rects().size() == 1);
    assert(rows.rects()[0] == (Box{0, 0, 40, 20}));

    // Columns too, and in an order that never grows the region monotonically.
    Region cols;
    for (int x : {3, 0, 4, 1, 2}) {
        cols.add(Box{x, 0, 1, 8});
    }
    assert(cols.rects().size() == 1);
    assert(cols.rects()[0] == (Box{0, 0, 5, 8}));

    // The four quadrants of a square, added corner-first, are one box.
    Region quads;
    quads.add(Box{0, 0, 5, 5});
    quads.add(Box{5, 5, 5, 5});
    quads.add(Box{5, 0, 5, 5});
    quads.add(Box{0, 5, 5, 5});
    assert(quads.rects().size() == 1);
    assert(quads.rects()[0] == (Box{0, 0, 10, 10}));

    // Boxes that only touch at a corner, or overlap on part of an edge, are NOT
    // one rectangle and must stay separate — merging them would claim pixels the
    // region does not cover.
    Region corner;
    corner.add(Box{0, 0, 5, 5});
    corner.add(Box{5, 5, 5, 5});
    assert(corner.rects().size() == 2);
    assert(area(corner) == 50);
    assert(!corner.contains(7, 2));

    Region partial;
    partial.add(Box{0, 0, 10, 5});
    partial.add(Box{0, 5, 4, 5}); // shares only part of the bottom edge
    assert(partial.rects().size() == 2);
    assert(area(partial) == 70);
    assert(!partial.contains(6, 7));

    // Punching a hole and filling it back in returns to a single box.
    Region refilled{Box{0, 0, 12, 12}};
    refilled.subtract(Box{4, 4, 4, 4});
    assert(refilled.rects().size() > 1);
    refilled.add(Box{4, 4, 4, 4});
    assert(refilled.rects().size() == 1);
    assert(refilled.rects()[0] == (Box{0, 0, 12, 12}));

    // Intersect coalesces too: two boxes clipped down to a shared edge rejoin.
    Region clip2;
    clip2.add(Box{0, 0, 10, 10});
    clip2.add(Box{10, 0, 10, 10});
    assert(clip2.rects().size() == 1); // already one on the way in
    clip2.intersect(Box{5, 0, 10, 10});
    assert(clip2.rects().size() == 1);
    assert(clip2.rects()[0] == (Box{5, 0, 10, 10}));

    // --- transform composition, which is what folds a client's
    //     set_buffer_transform into the output's own rotation ---
    using luminaria::Transform;
    assert(transform_compose(Transform::normal, Transform::rotate_90) == Transform::rotate_90);
    assert(transform_compose(Transform::rotate_90, Transform::rotate_90) == Transform::rotate_180);
    assert(transform_compose(Transform::rotate_270, Transform::rotate_90) == Transform::normal);
    // Composing with the inverse is the identity, for every transform.
    for (int i = 0; i < 8; ++i) {
        const auto t = static_cast<Transform>(i);
        assert(transform_compose(t, transform_invert(t)) == Transform::normal);
        assert(transform_compose(transform_invert(t), t) == Transform::normal);
    }
    return 0;
}
