// luminaria/shell/popup.cppm — popup hierarchy, placement and menu-grab routing.
export module luminaria:popup;

import std;

import :box;
import :handle;
import :scene;
import :signal;
import :xdg_shell;

export namespace luminaria {

/// The tree changed: a popup mapped, unmapped, moved or went away. Nothing a
/// client reports as damage, so the outputs need a frame.
struct PopupTreeChange {};

/// Every xdg_popup a shell creates, from creation to destruction, placed above
/// its parent by `append_to()` and dismissed by a click outside it.
///
/// It follows the shell by itself, so a compositor only draws it and asks for
/// a frame on `changed`. A popup whose parent is not an xdg surface — a layer
/// surface's, which gets its parent through `zwlr_layer_surface_v1.get_popup`
/// after creation — is given its parent with `set_parent()`.
///
/// Not movable: it watches the shell and every popup on its own behalf.
class PopupTree {
public:
    explicit PopupTree(XdgShell& shell);
    PopupTree(const PopupTree&) = delete;
    PopupTree& operator=(const PopupTree&) = delete;

    void set_parent(Popup& popup, SurfaceId parent);

    /// Append mapped popups above their parents in `scene`. Submenus nest
    /// correctly because creation order is parent-first and each appended
    /// popup becomes an origin.
    void append_to(std::vector<SceneItem>& scene) const;
    [[nodiscard]] bool contains(SurfaceId surface) const;
    [[nodiscard]] Popup* active_grab() const noexcept;
    /// Dismiss active grabs when `surface` is outside every popup in the tree.
    /// Returns true if a grab was dismissed (meaning the caller should swallow the click).
    bool dismiss_grabs_outside(SurfaceId surface);

    Signal<PopupTreeChange> changed;

private:
    struct Entry {
        Popup* popup;
        SurfaceId parent;
        bool mapped = false;
        Signal<PopupMap>::Connection on_map;
        Signal<PopupUnmap>::Connection on_unmap;
        Signal<PopupReposition>::Connection on_reposition;
        Signal<PopupDestroy>::Connection on_destroy;
    };
    void add(Popup& popup);
    void remove(Popup& popup);
    void set_mapped(Popup& popup, bool mapped);
    void notify();
    [[nodiscard]] Entry* find(Popup& popup) noexcept;
    std::vector<Entry> entries_;
    Signal<NewPopup>::Connection on_new_;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

PopupTree::Entry* PopupTree::find(Popup& popup) noexcept {
    const auto hit = std::ranges::find(entries_, &popup, &Entry::popup);
    return hit == entries_.end() ? nullptr : &*hit;
}

PopupTree::PopupTree(XdgShell& shell) {
    on_new_ = shell.new_popup().connect([this](NewPopup& e) { add(e.popup); });
}

void PopupTree::notify() {
    PopupTreeChange change;
    changed.emit(change);
}

void PopupTree::add(Popup& popup) {
    if (find(popup) != nullptr) {
        return;
    }
    const SurfaceId parent =
        popup.parent_surface() != nullptr ? popup.parent_surface()->id() : SurfaceId{};
    Entry& entry = entries_.emplace_back(Entry{&popup, parent, popup.mapped(), {}, {}, {}, {}});
    // By popup, never by entry: `entries_` reallocates as popups come and go.
    entry.on_map = popup.map.connect([this](PopupMap& e) { set_mapped(e.popup, true); });
    entry.on_unmap = popup.unmap.connect([this](PopupUnmap& e) { set_mapped(e.popup, false); });
    entry.on_reposition = popup.reposition.connect([this](PopupReposition&) { notify(); });
    entry.on_destroy = popup.destroy.connect([this](PopupDestroy& e) { remove(e.popup); });
}

void PopupTree::set_parent(Popup& popup, SurfaceId parent) {
    if (Entry* entry = find(popup); entry != nullptr) {
        entry->parent = parent;
        notify();
    }
}

void PopupTree::set_mapped(Popup& popup, bool mapped) {
    if (Entry* entry = find(popup); entry != nullptr) {
        entry->mapped = mapped;
        notify();
    }
}

void PopupTree::remove(Popup& popup) {
    // Erasing the entry inside its own destroy slot is safe: a Signal never
    // frees the slot that is running.
    if (std::erase_if(entries_, [&popup](const Entry& entry) { return entry.popup == &popup; }) >
        0) {
        notify();
    }
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
        const int w = entry.popup->width() > 0 ? entry.popup->width()
                                               : entry.popup->surface().surface_width();
        const int h = entry.popup->height() > 0 ? entry.popup->height()
                                                : entry.popup->surface().surface_height();
        scene.push_back({.kind = SceneItem::Kind::surface,
                         .box = {x, y, w, h},
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
