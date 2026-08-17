// luminaria/shell/cursor_manager.cppm — cursor image resolution, shape and constraint pipeline.
export module luminaria:cursor_manager;

import std;

import :box;
import :cursor_shape;
import :cursor_theme;
import :expected;
import :handle;
import :pointer;
import :pointer_constraints;
import :relative_pointer;
import :scene;
import :seat;

export namespace luminaria {

/// Manages cursor sprite resolution (protocol shape request vs client surface vs theme fallback),
/// pointer constraint boundaries, and relative motion dispatching.
class CursorManager {
public:
    CursorManager() = default;
    explicit CursorManager(CursorTheme theme) noexcept : theme_(std::move(theme)) {}

    /// Load the system/environment cursor theme.
    [[nodiscard]] static Result<CursorManager> create();

    void set_shape(std::string_view name) {
        cursor_name_ = std::string(name);
        cursor_uses_shape_ = true;
    }

    void set_surface_cursor() noexcept {
        cursor_uses_shape_ = false;
    }

    [[nodiscard]] const std::string& current_shape() const noexcept { return cursor_name_; }
    [[nodiscard]] bool uses_shape() const noexcept { return cursor_uses_shape_; }
    [[nodiscard]] const std::optional<CursorTheme>& theme() const noexcept { return theme_; }

    [[nodiscard]] const CursorImage* current_frame(std::uint32_t time_ms = 0) const noexcept;

    /// Resolve current cursor sprite to be drawn by SceneRenderer.
    [[nodiscard]] SceneCursor sprite(const Pointer& pointer, const Seat& seat) const noexcept;


    /// Calculate allowable cursor bounding box for `output_full`.
    /// Returns an empty box if the pointer is locked, surface box if confined, or `output_full`.
    [[nodiscard]] static Box pointer_bounds(const PointerConstraints* constraints,
                                            std::span<const SceneItem> scene,
                                            const Box& output_full) noexcept;

    /// Forward relative pointer motion delta with microsecond clock timestamp.
    static void send_relative_motion(RelativePointerManager* relative, double dx, double dy);

private:
    mutable std::optional<CursorTheme> theme_;
    std::string cursor_name_ = "default";
    bool cursor_uses_shape_ = false;

};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

Result<CursorManager> CursorManager::create() {
    Result<CursorTheme> theme = CursorTheme::load();
    if (!theme) {
        return std::unexpected(theme.error());
    }
    return CursorManager{std::move(*theme)};
}

const CursorImage* CursorManager::current_frame(std::uint32_t time_ms) const noexcept {
    return theme_.has_value() ? theme_->frame(cursor_name_, time_ms) : nullptr;
}

SceneCursor CursorManager::sprite(const Pointer& pointer, const Seat& seat) const noexcept {

    if (!pointer.present() || (!cursor_uses_shape_ && seat.cursor_hidden())) {
        return {};
    }
    if (!cursor_uses_shape_) {
        if (Surface* surface = surface_from_id(seat.cursor_surface()); surface != nullptr) {
            return {surface->id(), nullptr,
                    static_cast<int>(pointer.x()) - seat.cursor_hotspot_x(),
                    static_cast<int>(pointer.y()) - seat.cursor_hotspot_y()};
        }
    }
    const CursorImage* image =
        theme_.has_value() ? theme_->frame(cursor_name_, 0) : nullptr;
    return {{}, image,
            static_cast<int>(pointer.x()) - (image != nullptr ? image->hotspot_x : 0),
            static_cast<int>(pointer.y()) - (image != nullptr ? image->hotspot_y : 0)};
}

Box CursorManager::pointer_bounds(const PointerConstraints* constraints,
                                  std::span<const SceneItem> scene,
                                  const Box& output_full) noexcept {
    if (constraints == nullptr) {
        return output_full;
    }
    const PointerConstraint* active = constraints->active_constraint();

    if (active == nullptr) {
        return output_full;
    }
    if (active->type() == PointerConstraintType::locked) {
        return {};
    }
    const auto found =
        std::ranges::find(scene, active->surface().id(), &SceneItem::surface);
    return found == scene.end() ? output_full : found->box;
}

void CursorManager::send_relative_motion(RelativePointerManager* relative, double dx, double dy) {
    if (relative == nullptr || (dx == 0.0 && dy == 0.0)) {
        return;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto time_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    relative->send_motion(time_us, dx, dy, dx, dy);
}

} // namespace luminaria
