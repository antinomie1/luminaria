// luminaria/core/input_router.cppm — key event routing and modifier synchronization helpers.
export module luminaria:input_router;

import std;

import :input_event;
import :keymap;
import :seat;

export namespace luminaria {

/// Tracks swallowed keypresses to ensure matching key releases are also swallowed,
/// and provides helpers for synchronizing modifiers with Seat and releasing modifiers on VT switches.
class KeyRouter {
public:
    KeyRouter() = default;

    /// Record a keycode as swallowed by a binding/action.
    void consume(std::uint32_t keycode) {
        consumed_.insert(keycode);
    }

    /// Check if a release event should be swallowed because its press was swallowed.
    /// Erases the recorded keycode and returns true if it was swallowed.
    bool unconsume(std::uint32_t keycode) noexcept {
        return consumed_.erase(keycode) > 0;
    }

    [[nodiscard]] bool is_consumed(std::uint32_t keycode) const noexcept {
        return consumed_.contains(keycode);
    }

    /// Synchronize modifier state from backend input event into both keymap and seat.
    static void sync_modifiers(KeymapState& keymap, Seat& seat, const ModifiersEvent& e) {
        keymap.update_modifiers(e);
        seat.notify_modifiers(e.depressed, e.latched, e.locked, e.group);
    }

    /// Release held modifier keys to prevent clients from getting stuck keys across VT switches.
    static void release_held_modifiers(KeymapState& keymap, Seat& seat,
                                       const std::set<std::uint32_t>& held_modifiers) {
        for (const std::uint32_t modifier : held_modifiers) {
            keymap.update_key(modifier, false);
            seat.notify_key(modifier, false);
        }
        seat.notify_modifiers(0, 0, 0, 0);
    }

private:
    std::set<std::uint32_t> consumed_;
};

} // namespace luminaria
