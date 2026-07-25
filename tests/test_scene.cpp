// Phase 2 scene graph: tree structure, positioning, raise, and hit-testing.
// Pure logic — no client, no GPU. Also covers damage culling on flatten.
#include <cassert>

#include "luminaria/scene.hpp"

using namespace luminaria;

int main() {
    Scene scene;
    SceneTree& root = scene.root();

    // Two overlapping rects; b is added last -> on top.
    SceneRect& a = root.add_rect(100, 100, Color{1, 0, 0, 1});
    a.set_position(0, 0);
    SceneRect& b = root.add_rect(100, 100, Color{0, 1, 0, 1});
    b.set_position(50, 50);

    // Point in the overlap (60,60) hits the top node (b), local coords (10,10).
    auto hit = scene_node_at(root, 60, 60);
    assert(hit.has_value());
    assert(hit->node == &b);
    assert(hit->sx == 10 && hit->sy == 10);

    // Point only over a (10,10) hits a.
    auto hit_a = scene_node_at(root, 10, 10);
    assert(hit_a.has_value());
    assert(hit_a->node == &a);

    // Empty space misses.
    assert(!scene_node_at(root, 500, 500).has_value());

    // Raise a to the top; now the overlap hits a.
    a.raise_to_top();
    auto hit2 = scene_node_at(root, 60, 60);
    assert(hit2.has_value());
    assert(hit2->node == &a);
    assert(hit2->sx == 60 && hit2->sy == 60);

    // Nested tree offsets compose: child tree at (200,200), rect at (5,5) inside.
    SceneTree& child = root.add_tree();
    child.set_position(200, 200);
    SceneRect& c = child.add_rect(20, 20, Color{0, 0, 1, 1});
    c.set_position(5, 5);
    int ax = 0, ay = 0;
    c.absolute(ax, ay);
    assert(ax == 205 && ay == 205);
    auto hit_c = scene_node_at(root, 210, 210);
    assert(hit_c.has_value());
    assert(hit_c->node == &c);
    assert(hit_c->sx == 5 && hit_c->sy == 5);

    // --- damage culling: flattening with a region drops what did not change ---
    const std::vector<RectFill> all = scene_rects(root);
    assert(all.size() == 3);
    // An empty region means "no damage information", so nothing is dropped.
    assert(scene_rects(root, Region{}).size() == all.size());
    // A region covering only the nested rect leaves exactly that one.
    const std::vector<RectFill> culled = scene_rects(root, Region{Box{205, 205, 4, 4}});
    assert(culled.size() == 1);
    assert((culled.front().box == Box{205, 205, 20, 20}));
    // A region covering nothing leaves nothing.
    assert(scene_rects(root, Region{Box{9000, 9000, 10, 10}}).empty());

    return 0;
}
