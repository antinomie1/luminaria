// luminaria/input_event.cppm — backend-neutral input events. Both the libinput
// (bare-metal) and nested-Wayland backends are an `InputSource`: one signal
// carrying one `InputEvent`, so a compositor routes input into its Seat once,
// whichever backend it got. Keycodes/buttons are raw evdev (what
// wl_keyboard/wl_pointer expect), so they pass straight through.

module;

#include <cstdint>

export module luminaria:input_event;

import std;

import :output;
import :signal;

export namespace luminaria {

struct ModifiersEvent {
    uint32_t depressed, latched, locked, group; // xkb modifier masks + layout group
};
// `keysym` and `modifiers` are the backend's own reading of the key, against
// the keymap `InputSource::keymap()` returns and in the state from BEFORE this
// key: Logo then Return arrives as Return with Logo in `modifiers`, and Shift
// itself arrives without Shift. That is what a binding matches against, so a
// compositor needs no xkb state of its own to have shortcuts.
struct KeyEvent {
    uint32_t keycode; // evdev code (KEY_ESC == 1)
    bool pressed;
    uint32_t keysym = 0; // xkb keysym, 0 when nothing maps
    ModifiersEvent modifiers{};
};
// The layout the backend now reads keys against. Hand `text` to
// `Seat::set_keymap()`: the masks in `ModifiersEvent` are computed against it,
// and clients should see the layout the user types on. Borrowed for the emit.
struct KeymapChange {
    std::string_view text;
};
struct PointerMotionEvent {
    double dx, dy; // relative (libinput)
};
struct PointerButtonEvent {
    uint32_t button; // evdev code (BTN_LEFT == 272)
    bool pressed;
};
// Absolute pointer position in `output`'s own coordinates (nested backend: the
// parent reports where the cursor is over that output's window).
struct PointerMotionAbsEvent {
    double x, y;
    Output* output = nullptr;
};
// The pointer left every output (nested backend: it left our windows).
struct PointerLeaveEvent {};
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

using InputEvent = std::variant<KeyEvent, ModifiersEvent, KeymapChange, PointerMotionEvent,
                                PointerMotionAbsEvent, PointerLeaveEvent, PointerButtonEvent,
                                PointerAxisEvent, TouchDownEvent, TouchMotionEvent, TouchUpEvent,
                                TouchFrameEvent, TouchCancelEvent, InputCapabilities>;

/// Where input comes from: `LibinputBackend` on bare metal, `WaylandBackend`
/// nested. What differs between them — relative or absolute motion, touch or
/// none — is which alternatives of `InputEvent` arrive, not how they arrive.
class InputSource {
public:
    InputSource() = default;
    virtual ~InputSource() = default;
    InputSource(InputSource&&) = default;
    InputSource& operator=(InputSource&&) = default;
    InputSource(const InputSource&) = delete;
    InputSource& operator=(const InputSource&) = delete;

    /// Every input event, in the order the devices produced them.
    [[nodiscard]] virtual Signal<InputEvent>& input() noexcept = 0;
    /// The keymap `KeyEvent` and `ModifiersEvent` are read against, in text
    /// form; empty while there is none yet. Later changes arrive as
    /// `KeymapChange`.
    [[nodiscard]] virtual const std::string& keymap() const noexcept = 0;
    /// What the devices can do now. Later changes arrive as `InputCapabilities`.
    [[nodiscard]] virtual InputCapabilities capabilities() const noexcept = 0;
};

} // namespace luminaria
