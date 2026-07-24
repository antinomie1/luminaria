// luminaria/input_event.hpp — backend-neutral input events. Both the libinput
// (bare-metal) and nested-Wayland backends emit these; the compositor routes
// them into a Seat. Keycodes/buttons are raw evdev (what wl_keyboard/wl_pointer
// expect), so they pass straight through.
#pragma once

#include <cstdint>

namespace luminaria {

struct KeyEvent {
    uint32_t keycode; // evdev code (KEY_ESC == 1)
    bool pressed;
};
struct ModifiersEvent {
    uint32_t depressed, latched, locked, group; // xkb modifier masks + layout group
};
struct PointerMotionEvent {
    double dx, dy; // relative (libinput)
};
struct PointerButtonEvent {
    uint32_t button; // evdev code (BTN_LEFT == 272)
    bool pressed;
};
// Absolute pointer position in output-local coordinates (nested backend: the
// parent reports where the cursor is over our window).
struct PointerMotionAbsEvent {
    double x, y;
};
// One scroll frame. `dx/dy` are smooth (touchpad / hi-res wheel) deltas in
// surface units, positive = right/down. `dx_steps/dy_steps` count wheel notches
// and are 0 for touchpads. `stop_*` marks the end of a smooth scroll sequence.
struct PointerAxisEvent {
    double dx = 0.0, dy = 0.0;
    int32_t dx_steps = 0, dy_steps = 0;
    bool stop_horizontal = false, stop_vertical = false;
};

} // namespace luminaria
