// luminaria/scene.hpp — retained-mode scene graph.
//
// A tree of nodes the compositor arranges instead of hand-rolling render order,
// hit-testing, and damage. Node kinds: Tree (container), Rect (solid color),
// Surface (a client wl_surface). Children are ordered back-to-front (last = top).
//
// This slice provides the tree + positioning + hit-testing (what input routing
// needs). Rendering the tree via the Vulkan renderer plugs in when we composite.
// TODO: no damage tracking yet — add when per-frame repaint cost demands it.
#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "luminaria/util/color.hpp"
#include "luminaria/util/rect_fill.hpp"

namespace luminaria {

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
/// coordinates, ready for the renderer. (Surface nodes are skipped until the
/// renderer can sample client buffers.)
[[nodiscard]] std::vector<RectFill> scene_rects(const SceneTree& root);

/// Root of a scene.
class Scene {
public:
    [[nodiscard]] SceneTree& root() noexcept { return root_; }

private:
    SceneTree root_;
};

} // namespace luminaria
