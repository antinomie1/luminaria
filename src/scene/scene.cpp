#include "luminaria/scene.hpp"

#include <algorithm>

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
