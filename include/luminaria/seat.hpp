// luminaria/seat.hpp — the wl_seat: keyboard + pointer input routing.
//
// The seat advertises input capabilities and routes events to the focused
// client. Keyboard focus is surface-scoped: keys go to whoever holds focus.
// Pointer focus follows the cursor. The compositor decides focus (from scene
// hit-testing) and calls the notify_* methods; the seat handles the wire events.
//
// TODO: touch + drag-and-drop + grabs deferred until a client needs them.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "luminaria/core/expected.hpp"

namespace luminaria {

class Display;
class Surface;

class Seat {
public:
    /// Create the wl_seat global (keyboard + pointer capabilities) with an xkb
    /// keymap. Fails if the keymap can't be built.
    [[nodiscard]] static Result<Seat> create(Display& display, std::string name = "seat0");

    ~Seat();
    Seat(Seat&&) noexcept;
    Seat& operator=(Seat&&) noexcept;
    Seat(const Seat&) = delete;
    Seat& operator=(const Seat&) = delete;

    // --- keyboard ---
    /// Give keyboard focus to `surface` (nullptr clears focus). Sends leave/enter.
    void set_keyboard_focus(Surface* surface);
    /// Send a key event to the keyboard-focused client. `pressed` = down.
    void notify_key(uint32_t key, bool pressed);
    /// Send the current modifier state (Shift/Ctrl/…) to the keyboard-focused client.
    void notify_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);

    // --- pointer ---
    /// Move pointer focus onto `surface` at local (sx,sy) and send enter.
    void pointer_enter(Surface& surface, double sx, double sy);
    /// Send motion to the pointer-focused client.
    void pointer_motion(double sx, double sy);
    /// Send a button event to the pointer-focused client. `pressed` = down.
    void pointer_button(uint32_t button, bool pressed);

    struct Impl; // named by the protocol glue

private:
    std::unique_ptr<Impl> impl_;
    explicit Seat(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
