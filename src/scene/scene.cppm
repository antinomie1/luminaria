// luminaria/scene.cppm — retained-mode scene graph.
//
// A tree of nodes the compositor arranges instead of hand-rolling render order,
// hit-testing, and damage. Node kinds: Tree (container), Rect (solid color),
// Surface (a client wl_surface). Children are ordered back-to-front (last = top).
//
// The tree does positioning, hit-testing, flattening for the renderer, and
// damage: `scene_damage()` gathers what the clients said changed, and the
// flatten functions take that region and skip everything outside it.

module;

#include <memory>
#include <optional>
#include <vector>

#include <algorithm>

export module luminaria:scene;

import :box;
import :color;
import :compositor;
import :rect_fill;
import :region;
import :vulkan;

export namespace luminaria {

class Surface;
class SceneTree;

class SceneNode {
public:
    enum class Type { Tree, Rect, Surface };

    virtual ~SceneNode() = default;
    SceneNode(const SceneNode&) = delete;
    SceneNode& operator=(const SceneNode&) = delete;

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] int x() const noexcept { return x_; }
    [[nodiscard]] int y() const noexcept { return y_; }
    [[nodiscard]] SceneTree* parent() const noexcept { return parent_; }

    void set_position(int x, int y) noexcept {
        x_ = x;
        y_ = y;
    }

    /// Absolute position in the scene (sum of offsets up to the root).
    void absolute(int& out_x, int& out_y) const noexcept;

    /// Move this node to the top of its parent's child order.
    void raise_to_top() noexcept;

protected:
    explicit SceneNode(Type type) noexcept : type_(type) {}

    Type type_;
    int x_ = 0;
    int y_ = 0;
    SceneTree* parent_ = nullptr;
    friend class SceneTree;
};

/// A node with pixel extent (Rect or Surface).
class SceneLeaf : public SceneNode {
public:
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

    /// True if (lx,ly), given in this node's local coordinates, is inside it.
    [[nodiscard]] bool local_contains(int lx, int ly) const noexcept {
        return lx >= 0 && ly >= 0 && lx < width_ && ly < height_;
    }

protected:
    SceneLeaf(Type type, int width, int height) noexcept
        : SceneNode(type), width_(width), height_(height) {}
    int width_;
    int height_;
};

class SceneRect final : public SceneLeaf {
public:
    SceneRect(int width, int height, Color color) noexcept
        : SceneLeaf(Type::Rect, width, height), color_(color) {}
    [[nodiscard]] Color color() const noexcept { return color_; }

private:
    Color color_;
};

class SceneSurface final : public SceneLeaf {
public:
    SceneSurface(Surface& surface, int width, int height) noexcept
        : SceneLeaf(Type::Surface, width, height), surface_(&surface) {}
    [[nodiscard]] Surface& surface() const noexcept { return *surface_; }

private:
    Surface* surface_;
};

class SceneTree final : public SceneNode {
public:
    SceneTree() noexcept : SceneNode(Type::Tree) {}

    SceneTree& add_tree();
    SceneRect& add_rect(int width, int height, Color color);
    SceneSurface& add_surface(Surface& surface, int width, int height);

    [[nodiscard]] const std::vector<std::unique_ptr<SceneNode>>& children() const noexcept {
        return children_;
    }

private:
    std::vector<std::unique_ptr<SceneNode>> children_; // back-to-front
    friend class SceneNode;
};

/// A hit-test result: the topmost leaf under a point, and the point in that
/// leaf's local coordinates.
struct SceneHit {
    SceneLeaf* node;
    int sx;
    int sy;
};

/// Topmost leaf at (lx,ly), expressed in `tree`'s local coordinates.
[[nodiscard]] std::optional<SceneHit> scene_node_at(const SceneTree& tree, int lx, int ly);

/// Flatten the tree's Rect nodes to back-to-front RectFills in absolute
/// coordinates, ready for the renderer. Surface nodes go through
/// `scene_textures()` instead.
[[nodiscard]] std::vector<RectFill> scene_rects(const SceneTree& root);

/// The same, but only the rects that intersect `damage`. An empty region means
/// nothing is dropped — pass `scene_damage()` and a frame where one client
/// blinked a cursor costs one small quad instead of the whole tree.
[[nodiscard]] std::vector<RectFill> scene_rects(const SceneTree& root, const Region& damage);

/// Flatten the tree's Surface nodes to GpuTextureFills in absolute coordinates,
/// back-to-front, importing each client buffer through the surface's texture
/// cache. Surfaces with no usable buffer, and (when `damage` is non-empty) those
/// outside it, are skipped. `renderer` must outlive the surfaces.
[[nodiscard]] std::vector<GpuTextureFill> scene_textures(const SceneTree& root,
                                                          VulkanRenderer& renderer,
                                                          const Region& damage = {});

/// Everything the clients in this tree reported as changed, in absolute
/// coordinates and with no overlaps. Feed it to the flatten functions above and
/// to `VulkanRenderer::render_to`, then call `scene_clear_damage()`.
[[nodiscard]] Region scene_damage(const SceneTree& root);

/// Damage consumed: start accumulating again. Call it once the frame carrying
/// it has been submitted.
void scene_clear_damage(const SceneTree& root);

/// Root of a scene.
class Scene {
public:
    [[nodiscard]] SceneTree& root() noexcept { return root_; }

private:
    SceneTree root_;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

void SceneNode::absolute(int& out_x, int& out_y) const noexcept {
    int ax = 0;
    int ay = 0;
    for (const SceneNode* n = this; n != nullptr; n = n->parent_) {
        ax += n->x_;
        ay += n->y_;
    }
    out_x = ax;
    out_y = ay;
}

void SceneNode::raise_to_top() noexcept {
    if (parent_ == nullptr) {
        return;
    }
    auto& siblings = parent_->children_;
    auto it = std::ranges::find_if(siblings,
                                   [this](const std::unique_ptr<SceneNode>& p) { return p.get() == this; });
    if (it != siblings.end() && std::next(it) != siblings.end()) {
        std::rotate(it, std::next(it), siblings.end()); // move *it to the back (top)
    }
}

SceneTree& SceneTree::add_tree() {
    auto node = std::make_unique<SceneTree>();
    node->parent_ = this;
    SceneTree& ref = *node;
    children_.push_back(std::move(node));
    return ref;
}

SceneRect& SceneTree::add_rect(int width, int height, Color color) {
    auto node = std::make_unique<SceneRect>(width, height, color);
    node->parent_ = this;
    SceneRect& ref = *node;
    children_.push_back(std::move(node));
    return ref;
}

SceneSurface& SceneTree::add_surface(Surface& surface, int width, int height) {
    auto node = std::make_unique<SceneSurface>(surface, width, height);
    node->parent_ = this;
    SceneSurface& ref = *node;
    children_.push_back(std::move(node));
    return ref;
}

namespace {
void collect_rects(const SceneTree& tree, int ox, int oy, std::vector<RectFill>& out) {
    for (const auto& child : tree.children()) { // back-to-front
        const int cx = ox + child->x();
        const int cy = oy + child->y();
        if (child->type() == SceneNode::Type::Tree) {
            collect_rects(static_cast<const SceneTree&>(*child), cx, cy, out);
        } else if (child->type() == SceneNode::Type::Rect) {
            const auto& rect = static_cast<const SceneRect&>(*child);
            out.push_back(RectFill{Box{cx, cy, rect.width(), rect.height()}, rect.color()});
        }
    }
}
} // namespace

std::vector<RectFill> scene_rects(const SceneTree& root) {
    std::vector<RectFill> out;
    collect_rects(root, 0, 0, out);
    return out;
}

std::vector<RectFill> scene_rects(const SceneTree& root, const Region& damage) {
    std::vector<RectFill> out = scene_rects(root);
    if (damage.empty()) {
        return out; // no damage information: everything is suspect
    }
    std::erase_if(out, [&damage](const RectFill& r) { return !damage.intersects(r.box); });
    return out;
}

namespace {

void collect_textures(const SceneTree& tree, int ox, int oy, VulkanRenderer& renderer,
                      const Region& damage, std::vector<GpuTextureFill>& out) {
    for (const auto& child : tree.children()) { // back-to-front
        const int cx = ox + child->x();
        const int cy = oy + child->y();
        if (child->type() == SceneNode::Type::Tree) {
            collect_textures(static_cast<const SceneTree&>(*child), cx, cy, renderer, damage, out);
            continue;
        }
        if (child->type() != SceneNode::Type::Surface) {
            continue;
        }
        const auto& node = static_cast<const SceneSurface&>(*child);
        Surface& surface = node.surface();
        const Box box{cx, cy, surface.surface_width(), surface.surface_height()};
        if (box.empty() || (!damage.empty() && !damage.intersects(box))) {
            continue;
        }
        GpuTexture* texture = surface.buffer_texture(renderer);
        if (texture == nullptr) {
            continue;
        }
        GpuTextureFill fill{};
        fill.texture = texture;
        fill.x = box.x;
        fill.y = box.y;
        fill.w = box.width;
        fill.h = box.height;
        fill.transform = surface.buffer_transform();
        surface.buffer_source_uv(fill.u0, fill.v0, fill.u1, fill.v1);
        // What the client promised is opaque, moved into absolute coordinates —
        // the renderer skips whatever is behind it.
        const Box opaque = surface.opaque_region().extents();
        fill.opaque = Box{box.x + opaque.x, box.y + opaque.y, opaque.width, opaque.height};
        out.push_back(fill);
    }
}

/// Walk every Surface node, giving each one its absolute position.
template <class Fn>
void for_each_surface(const SceneTree& tree, int ox, int oy, Fn&& fn) {
    for (const auto& child : tree.children()) {
        const int cx = ox + child->x();
        const int cy = oy + child->y();
        if (child->type() == SceneNode::Type::Tree) {
            for_each_surface(static_cast<const SceneTree&>(*child), cx, cy, fn);
        } else if (child->type() == SceneNode::Type::Surface) {
            fn(static_cast<const SceneSurface&>(*child).surface(), cx, cy);
        }
    }
}

} // namespace

std::vector<GpuTextureFill> scene_textures(const SceneTree& root, VulkanRenderer& renderer,
                                            const Region& damage) {
    std::vector<GpuTextureFill> out;
    collect_textures(root, 0, 0, renderer, damage, out);
    return out;
}

Region scene_damage(const SceneTree& root) {
    Region region;
    for_each_surface(root, 0, 0, [&region](Surface& surface, int x, int y) {
        for (const Box& d : surface.damage()) {
            region.add(Box{x + d.x, y + d.y, d.width, d.height});
        }
    });
    return region;
}

void scene_clear_damage(const SceneTree& root) {
    for_each_surface(root, 0, 0, [](Surface& surface, int, int) { surface.clear_damage(); });
}

std::optional<SceneHit> scene_node_at(const SceneTree& tree, int lx, int ly) {
    // Front-to-back: the last child is topmost, so walk children in reverse.
    const auto& children = tree.children();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        SceneNode* child = it->get();
        const int cx = lx - child->x();
        const int cy = ly - child->y();
        if (child->type() == SceneNode::Type::Tree) {
            if (auto hit = scene_node_at(static_cast<SceneTree&>(*child), cx, cy)) {
                return hit;
            }
        } else {
            auto* leaf = static_cast<SceneLeaf*>(child);
            if (leaf->local_contains(cx, cy)) {
                return SceneHit{leaf, cx, cy};
            }
        }
    }
    return std::nullopt;
}

} // namespace luminaria
