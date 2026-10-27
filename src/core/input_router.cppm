// luminaria/core/input_router.cppm — the compositor's key bindings in front of the seat.
export module luminaria:input_router;

import std;

import :input_event;
import :seat;

export namespace luminaria {

/// Splits the keyboard between the compositor's bindings and the focused
/// client, keeping each key whole: a key whose press a binding took has its
/// release taken too, so the client never sees half of it.
class KeyRouter {
public:
    /// Route one backend key event. A press is offered to `binding` first
    /// (`bool(const KeyEvent&)`); answering true keeps the key from the client.
    /// Everything else goes to `seat`.
    ///
    /// A press of a key the router thinks a binding still holds means its
    /// release was lost — what happens to the key that switched the VT away —
    /// and is treated as a fresh press.
    template <class Binding>
    void route(Seat& seat, const KeyEvent& event, Binding&& binding) {
        if (!event.pressed) {
            if (consumed_.erase(event.keycode) == 0) {
                seat.notify_key(event.keycode, false);
            }
            return;
        }
        consumed_.erase(event.keycode);
        if (std::invoke(binding, event)) {
            consumed_.insert(event.keycode);
            return;
        }
        seat.notify_key(event.keycode, true);
    }

private:
    std::set<std::uint32_t> consumed_;
};

} // namespace luminaria
