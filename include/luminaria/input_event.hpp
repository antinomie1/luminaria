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

} // namespace luminaria
