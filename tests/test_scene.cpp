// The scene list's pure half: what is under a point, front to back.
//
// No display, no GPU, no client — the surface items need a live compositor and
// are covered by the compositors that drive one; everything a tiler actually
// hit-tests against between them (borders, solid rectangles, images that are
// drawn but never hit) is reachable here.
#include <cassert>

import luminaria;
import std;

using luminaria::SceneHit;
using luminaria::SceneItem;
using luminaria::scene_hit_test;

int main() {
    // A bar across the top that takes no input, then two windows' borders.
    const std::vector<SceneItem> scene{
        {.kind = SceneItem::Kind::image, .box = {0, 0, 800, 20}, .accepts_input = false},
        {.kind = SceneItem::Kind::border, .box = {0, 20, 400, 580}, .thickness = 2, .tag = 7},
        {.kind = SceneItem::Kind::border, .box = {400, 20, 400, 580}, .thickness = 2, .tag = 9},
        // A floating window over the second one: later in the list, so it wins.
        {.kind = SceneItem::Kind::rect, .box = {380, 100, 100, 100}, .tag = 11},
    };

    assert(!scene_hit_test(scene, 10.0, 5.0).hit); // the bar is drawn, never hit
    assert(scene_hit_test(scene, 10.0, 300.0).tag == 7);
    assert(scene_hit_test(scene, 500.0, 300.0).tag == 9);
    assert(scene_hit_test(scene, 390.0, 150.0).tag == 11); // topmost wins
    assert(scene_hit_test(scene, 390.0, 300.0).tag == 7);  // just below it
    assert(!scene_hit_test(scene, 900.0, 300.0).hit);      // off the right edge

    // A hit on something that is not a surface resolves to no surface at all,
    // which is the ordinary case of the cursor sitting on a window's frame.
    const SceneHit border = scene_hit_test(scene, 1.0, 21.0);
    assert(border.hit);
    assert(border.surface == nullptr);
    assert(border.sx == 0.0 && border.sy == 0.0);

    assert(!scene_hit_test({}, 0.0, 0.0).hit);
    return 0;
}
