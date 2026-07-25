// luminaria/scene.hpp — retained-mode scene graph.
//
// A tree of nodes the compositor arranges instead of hand-rolling render order,
// hit-testing, and damage. Node kinds: Tree (container), Rect (solid color),
// Surface (a client wl_surface). Children are ordered back-to-front (last = top).
//
// The tree does positioning, hit-testing, flattening for the renderer, and
// damage: `scene_damage()` gathers what the clients said changed, and the
// flatten functions take that region and skip everything outside it.
#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "luminaria/render/vulkan.hpp"
#include "luminaria/util/color.hpp"
#include "luminaria/util/rect_fill.hpp"
#include "luminaria/util/region.hpp"

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
