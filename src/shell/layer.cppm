// luminaria/shell/layer.cppm — multi-layer placement and exclusive-zone arrangement.
export module luminaria:layer;

import std;

import :box;
import :handle;
import :layer_shell;
import :scene;

export namespace luminaria {

/// Manages zwlr_layer_surface_v1 instances across outputs, handles exclusive-zone
/// subtraction, and provides layer-ordered scene item generation.
class LayerManager {
public:
    void add(LayerSurface& surface, std::uint32_t output_id);
    void remove(LayerSurface& surface) noexcept;
    void set_mapped(LayerSurface& surface, bool mapped) noexcept;

    /// Configure every surface on `output_id`, compute their bounds against `full`
    /// and `usable`, and return the usable area left after all positive exclusive zones.
    [[nodiscard]] Box arrange(std::uint32_t output_id, const Box& full, Box usable);

    /// Append mapped surfaces belonging to `layer` on `output_id` to `scene`.
    void append_to(std::vector<SceneItem>& scene, std::uint32_t output_id, Layer layer) const;

    /// Find which LayerSurface (if any) contains `surface` and accepts keyboard focus.
    [[nodiscard]] LayerSurface* keyboard_target(SurfaceId surface) const;

    /// Ask all surfaces on `output_id` to close (e.g. when an output is disconnected).
    void close_output(std::uint32_t output_id);

private:
    struct Entry {
        LayerSurface* surface;
        std::uint32_t output_id;
        Box box;
        bool mapped = false;
    };

    [[nodiscard]] Entry* find(LayerSurface& surface) noexcept;
    std::vector<Entry> entries_;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

LayerManager::Entry* LayerManager::find(LayerSurface& surface) noexcept {
    const auto hit = std::ranges::find(entries_, &surface, &Entry::surface);
    return hit == entries_.end() ? nullptr : &*hit;
}

void LayerManager::add(LayerSurface& surface, std::uint32_t output_id) {
    if (find(surface) == nullptr) {
        entries_.push_back({&surface, output_id, {}, false});
    }
}

void LayerManager::remove(LayerSurface& surface) noexcept {
    std::erase_if(entries_, [&surface](const Entry& entry) { return entry.surface == &surface; });
}

void LayerManager::set_mapped(LayerSurface& surface, bool mapped) noexcept {
    if (Entry* entry = find(surface); entry != nullptr) {
        entry->mapped = mapped;
    }
}

Box LayerManager::arrange(std::uint32_t output_id, const Box& full, Box usable) {
    std::vector<Entry*> ordered;
    for (Entry& entry : entries_) {
        if (entry.output_id == output_id) {
            ordered.push_back(&entry);
        }
    }
    std::ranges::stable_sort(ordered, [](const Entry* a, const Entry* b) {
        if (a->surface->layer() != b->surface->layer()) {
            return a->surface->layer() < b->surface->layer();
        }
        return (a->surface->exclusive_zone() > 0) > (b->surface->exclusive_zone() > 0);
    });

    for (Entry* entry : ordered) {
        Box next = usable;
        entry->box = arrange_layer_surface(*entry->surface, full, next);
        if (entry->mapped) {
            usable = next;
        }
    }
    return usable;
}

void LayerManager::append_to(std::vector<SceneItem>& scene, std::uint32_t output_id,
                             Layer layer) const {
    for (const Entry& entry : entries_) {
        if (entry.output_id != output_id || !entry.mapped || entry.surface->layer() != layer ||
            entry.box.empty()) {
            continue;
        }
        scene.push_back({.kind = SceneItem::Kind::surface,
                         .box = entry.box,
                         .surface = entry.surface->surface().id(),
                         .x = entry.box.x,
                         .y = entry.box.y});
    }
}

LayerSurface* LayerManager::keyboard_target(SurfaceId surface) const {
    for (const Entry& entry : entries_) {
        if (!entry.mapped || entry.surface->keyboard_interactivity() ==
                                 LayerKeyboardInteractivity::none) {
            continue;
        }
        for (const SurfaceAt& item : entry.surface->surface().surface_tree()) {
            if (item.surface->id() == surface) {
                return entry.surface;
            }
        }
    }
    return nullptr;
}

void LayerManager::close_output(std::uint32_t output_id) {
    for (const Entry& entry : entries_) {
        if (entry.output_id == output_id) {
            entry.surface->close();
        }
    }
}

} // namespace luminaria
