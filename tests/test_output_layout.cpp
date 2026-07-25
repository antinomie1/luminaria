// OutputLayout is pure geometry: where each output sits in the shared
// coordinate space, and what a window straddling two of them covers.
#include <cassert>

import luminaria;

int main() {
    auto display = luminaria::Display::create();
    assert(display.has_value());
    luminaria::HeadlessBackend backend{display->event_loop()};
    luminaria::Output& left = backend.add_output(800, 600);
    luminaria::Output& right = backend.add_output(1280, 720);

    // --- transform + scale maths, before any of it reaches the layout ---
    using luminaria::Box;
    using luminaria::Transform;
    // A 1920x1080 panel rotated 90 degrees is a 1080x1920 logical space.
    assert(transform_swaps_axes(Transform::rotate_90));
    assert(!transform_swaps_axes(Transform::rotate_180));
    assert(transform_flipped(Transform::flipped_270));
    assert(transform_invert(Transform::rotate_90) == Transform::rotate_270);
    assert(transform_invert(Transform::flipped_90) == Transform::flipped_90);

    // Scale only: one logical unit is two pixels.
    assert((transform_box(Transform::normal, 2, Box{1, 2, 3, 4}, 800, 600) == Box{2, 4, 6, 8}));
    // 180: the box lands mirrored in both axes.
    assert((transform_box(Transform::rotate_180, 1, Box{0, 0, 10, 20}, 100, 50) ==
            Box{90, 30, 10, 20}));
    // 90: axes swap, and the logical top-left corner lands top-right.
    assert((transform_box(Transform::rotate_90, 1, Box{0, 0, 10, 20}, 100, 50) ==
            Box{80, 0, 20, 10}));
    // 270 is the other way round.
    assert((transform_box(Transform::rotate_270, 1, Box{0, 0, 10, 20}, 100, 50) ==
            Box{0, 40, 20, 10}));
    // Flipped mirrors along logical x first; the whole width is 100 here.
    assert((transform_box(Transform::flipped, 1, Box{0, 0, 10, 20}, 100, 50) ==
            Box{90, 0, 10, 20}));
    // Rotating the full logical rect must cover the whole framebuffer.
    assert((transform_box(Transform::rotate_90, 2, Box{0, 0, 25, 50}, 100, 50) ==
            Box{0, 0, 100, 50}));

    luminaria::OutputLayout layout;
    assert(layout.empty());
    layout.add_auto(left);
    layout.add_auto(right);

    assert((layout.box_of(left) == luminaria::Box{0, 0, 800, 600}));
    assert((layout.box_of(right) == luminaria::Box{800, 0, 1280, 720}));
    assert((layout.bounds() == luminaria::Box{0, 0, 2080, 720}));

    assert(layout.at(10, 10) == &left);
    assert(layout.at(900, 10) == &right);
    assert(layout.at(10, 700) == nullptr); // below the shorter output: a gap

    // A window across the seam lands on both, clipped to each.
    const auto hits = layout.intersecting(luminaria::Box{700, 100, 300, 200});
    assert(hits.size() == 2);
    assert(hits[0].output == &left);
    assert((hits[0].box == luminaria::Box{700, 100, 100, 200}));
    assert(hits[1].output == &right);
    assert((hits[1].box == luminaria::Box{800, 100, 200, 200}));

    // A rotated, scaled output occupies its LOGICAL rectangle in the layout.
    right.set_transform(Transform::rotate_90);
    right.set_scale(2);
    assert(right.logical_width() == 720 / 2);
    assert(right.logical_height() == 1280 / 2);
    layout.add(right, 800, 0); // re-add to pick up the new size
    assert((layout.box_of(right) == Box{800, 0, 360, 640}));
    right.set_transform(Transform::normal);
    right.set_scale(1);
    layout.add(right, 800, 0);

    // Explicit placement overrides, and removal is complete.
    layout.add(right, 0, 600);
    assert((layout.box_of(right) == luminaria::Box{0, 600, 1280, 720}));
    layout.remove(right);
    assert(layout.outputs().size() == 1);
    assert((layout.box_of(right) == luminaria::Box{}));
    return 0;
}
