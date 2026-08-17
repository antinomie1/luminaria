// luminaria/shell/popup.cppm — popup hierarchy, placement and menu-grab routing.
export module luminaria:popup;

import std;

import :box;
import :handle;
import :scene;
import :xdg_shell;

export namespace luminaria {

/// Tracks active XDG popups, computes recursive scene placement relative to
/// parent surfaces, and handles grab dismissal on outside clicks.
class PopupTree {
public:
    void add(Popup& popup, SurfaceId parent = {});
    void set_parent(Popup& popup, SurfaceId parent);
    void set_mapped(Popup& popup, bool mapped) noexcept;
    void remove(Popup& popup) noexcept;

    /// Append mapped popups above their parents in `scene`. Submenus nest
    /// correctly because creation order is parent-first and each appended
    /// popup becomes an origin.
    void append_to(std::vector<SceneItem>& scene) const;

    [[nodiscard]] bool contains(SurfaceId surface) const;
    [[nodiscard]] Popup* active_grab() const noexcept;
    /// Dismiss active grabs when `surface` is outside every popup in the tree.
    /// Returns true if a grab was dismissed (meaning the caller should swallow the click).
    [[nodiscard]] bool dismiss_grabs_outside(SurfaceId surface);

private:
    struct Entry {
        Popup* popup;
        SurfaceId parent;
        bool mapped = false;
    };

    [[nodiscard]] Entry* find(Popup& popup) noexcept;
    std::vector<Entry> entries_;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

PopupTree::Entry* PopupTree::find(Popup& popup) noexcept {
    const auto hit = std::ranges::find(entries_, &popup, &Entry::popup);
    return hit == entries_.end() ? nullptr : &*hit;
}

void PopupTree::add(Popup& popup, SurfaceId parent) {
    if (Entry* entry = find(popup); entry != nullptr) {
        if (parent.valid()) {
            entry->parent = parent;
        }
        return;
    }
    if (!parent.valid() && popup.parent_surface() != nullptr) {
        parent = popup.parent_surface()->id();
    }
    entries_.push_back({&popup, parent, false});
}

void PopupTree::set_parent(Popup& popup, SurfaceId parent) {
    add(popup, parent);
}

void PopupTree::set_mapped(Popup& popup, bool mapped) noexcept {
    if (Entry* entry = find(popup); entry != nullptr) {
        entry->mapped = mapped;
    }
}

void PopupTree::remove(Popup& popup) noexcept {
    std::erase_if(entries_, [&popup](const Entry& entry) { return entry.popup == &popup; });
}

void PopupTree::append_to(std::vector<SceneItem>& scene) const {
    struct Origin {
        SurfaceId surface;
        int x, y;
        std::uint64_t tag;
    };
    std::vector<Origin> origins;
    origins.reserve(scene.size() + entries_.size());
    for (const SceneItem& item : scene) {
        if (item.kind == SceneItem::Kind::surface) {
            origins.push_back({item.surface, item.box.x, item.box.y, item.tag});
        }
    }

    for (const Entry& entry : entries_) {
        if (!entry.mapped || entry.popup == nullptr) {
            continue;
        }
        const auto parent = std::ranges::find(origins, entry.parent, &Origin::surface);
        if (parent == origins.end()) {
            continue;
        }
        const int x = parent->x + entry.popup->x();
        const int y = parent->y + entry.popup->y();
        const SurfaceId surface = entry.popup->surface().id();
        scene.push_back({.kind = SceneItem::Kind::surface,
                         .box = {x, y, entry.popup->width(), entry.popup->height()},
                         .surface = surface,
                         .x = x,
                         .y = y,
                         .tag = parent->tag});
        origins.push_back({surface, x, y, parent->tag});
    }
}

bool PopupTree::contains(SurfaceId surface) const {
    if (!surface.valid()) {
        return false;
    }
    for (const Entry& entry : entries_) {
        if (!entry.mapped || entry.popup == nullptr) {
            continue;
        }
        for (const SurfaceAt& item : entry.popup->surface().surface_tree()) {
            if (item.surface->id() == surface) {
                return true;
            }
        }
    }
    return false;
}

Popup* PopupTree::active_grab() const noexcept {
    for (const Entry& entry : std::views::reverse(entries_)) {
        if (entry.mapped && entry.popup != nullptr && entry.popup->has_grab()) {
            return entry.popup;
        }
    }
    return nullptr;
}

bool PopupTree::dismiss_grabs_outside(SurfaceId surface) {
    if (contains(surface)) {
        return false;
    }
    bool dismissed = false;
    for (const Entry& entry : entries_) {
        if (entry.popup != nullptr && entry.popup->has_grab()) {
            entry.popup->dismiss();
            dismissed = true;
        }
    }
    return dismissed;
}

} // namespace luminaria
