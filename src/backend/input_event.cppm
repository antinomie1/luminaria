// luminaria/input_event.cppm — backend-neutral input events. Both the libinput
// (bare-metal) and nested-Wayland backends emit these; the compositor routes
// them into a Seat. Keycodes/buttons are raw evdev (what wl_keyboard/wl_pointer
// expect), so they pass straight through.

module;

#include <cstdint>

export module luminaria:input_event;
export namespace luminaria {

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
// Touchscreen points. `id` is the seat-wide slot: unique among the points that
// are down at once across every touch device, and reused once a point lifts —
// what wl_touch calls the id. `x/y` are NORMALISED to the touchscreen: (0,0) is
// its top-left, (1,1) its bottom-right, with any calibration matrix udev gave
// the device already applied. The backend cannot know which output the panel
// sits on top of, so mapping them is the compositor's: scale by that output's
// box (rotating them back first if the output is transformed, see
// `transform_unit_point()`). Points can fall slightly outside [0,1] on a badly
// calibrated panel; clamp before hit-testing.
struct TouchDownEvent {
    int32_t id;
    double x, y;
};
struct TouchMotionEvent {
    int32_t id;
    double x, y;
};
struct TouchUpEvent {
    int32_t id;
};
// Everything since the previous frame happened at one device sample; send
// `Seat::touch_frame()` here, not after each point.
struct TouchFrameEvent {};
// The device stopped reporting this point (it was unplugged, or the session
// lost the console). It will never get an up; treat it as gone.
struct TouchCancelEvent {
    int32_t id;
};
// What kinds of device the backend currently has open. Hand it straight to
// `Seat::set_capabilities()`: a wl_seat that advertises a pointer no device can
// produce makes clients wait for events that never come, and one that hides a
// keyboard the user is typing on is worse.
struct InputCapabilities {
    bool keyboard = false, pointer = false, touch = false;

    bool operator==(const InputCapabilities&) const noexcept = default;
};

} // namespace luminaria
