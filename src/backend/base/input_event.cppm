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
// --- Graphics tablets -------------------------------------------------------
//
// A drawing tablet is two things the kernel reports separately: the TABLET (a
// surface a pen is held over) and the TOOL (the pen, an eraser, a puck), which
// moves between tablets and carries its own identity. Events name both: `device`
// is the tablet a tool is over, `tool` is the tool. They are backend-neutral ids,
// stable while the device or tool is known, and mean nothing outside this
// process.
//
// Positions are NORMALISED to the tablet's active area — (0,0) its top-left,
// (1,1) its bottom-right — for the same reason touch is: the backend cannot know
// which output the area maps to. `TabletManager::feed()` does that mapping.
// `TabletToolType` has the protocol's numeric values, which are the evdev
// BTN_TOOL_* codes.
enum class TabletToolType : uint32_t {
    pen = 0x140,
    eraser = 0x141,
    brush = 0x142,
    pencil = 0x143,
    airbrush = 0x144,
    finger = 0x145,
    mouse = 0x146,
    lens = 0x147,
};

/// Which axes a tool can report beyond position. Position and the buttons are
/// universal.
struct TabletToolCaps {
    bool tilt = false, pressure = false, distance = false;
    bool rotation = false, slider = false, wheel = false;

    bool operator==(const TabletToolCaps&) const noexcept = default;
};

/// One sample of a tool's axes. Only the axes named in `changed` are meaningful
/// in an update; a proximity-in carries all of them.
struct TabletAxes {
    enum : uint32_t {
        position = 1u << 0,
        pressure_axis = 1u << 1,
        distance_axis = 1u << 2,
        tilt = 1u << 3,
        rotation_axis = 1u << 4,
        slider_axis = 1u << 5,
        wheel_axis = 1u << 6,
        everything = 0x7fu,
    };
    uint32_t changed = 0;
    double x = 0.0, y = 0.0;         ///< normalised across the tablet
    double pressure = 0.0;           ///< 0..1
    double distance = 0.0;           ///< 0..1, how far above the surface
    double tilt_x = 0.0, tilt_y = 0.0; ///< degrees from the surface normal, -90..90
    double rotation = 0.0;           ///< degrees clockwise about the tool's axis
    double slider = 0.0;             ///< -1..1
    double wheel_degrees = 0.0;      ///< wheel / ring delta since the last sample
    int32_t wheel_clicks = 0;        ///< the same, in discrete notches
};

/// How a pad's controls are laid out. A group is the set of buttons, rings and
/// strips one mode switch governs; `modes` is how many modes it cycles through.
struct TabletPadGroup {
    uint32_t modes = 1;
    std::vector<uint32_t> buttons, rings, strips; ///< indexes into the pad's
};
struct TabletPadLayout {
    uint32_t buttons = 0, rings = 0, strips = 0;
    std::vector<TabletPadGroup> groups;
};

/// A tablet, or the pad of buttons and touch strips beside one, appeared. `bustype`
/// is the evdev BUS_* value, 0 when unknown.
struct TabletDeviceAdded {
    uint32_t device = 0;
    std::string name;
    uint16_t vid = 0, pid = 0;
    uint32_t bustype = 0;
    std::string path;          ///< "/dev/input/eventN"
    bool is_pad = false;       ///< a pad: `pad` describes it. Otherwise a tablet to draw on.
    TabletPadLayout pad;
};
struct TabletDeviceRemoved {
    uint32_t device = 0;
};
// A tool came into range of (or left) `device`. On `in`, `axes` has everything
// the tool reports at that moment.
struct TabletToolProximityEvent {
    uint32_t device = 0;
    uint64_t tool = 0;
    bool in = false;
    TabletToolType type = TabletToolType::pen;
    uint64_t serial = 0;    ///< the pen's hardware serial, 0 when it has none
    uint64_t wacom_id = 0;  ///< Wacom's tool id, 0 when unknown
    TabletToolCaps caps;
    TabletAxes axes;
    uint32_t time_ms = 0;
};
struct TabletToolAxisEvent {
    uint32_t device = 0;
    uint64_t tool = 0;
    TabletAxes axes;
    uint32_t time_ms = 0;
};
// The tip touched the surface (`down`) or lifted. Hover is proximity without it.
struct TabletToolTipEvent {
    uint32_t device = 0;
    uint64_t tool = 0;
    bool down = false;
    TabletAxes axes;
    uint32_t time_ms = 0;
};
// A button on the pen's barrel (BTN_STYLUS, BTN_STYLUS2, BTN_STYLUS3) or a mouse
// puck's.
struct TabletToolButtonEvent {
    uint32_t device = 0;
    uint64_t tool = 0;
    uint32_t button = 0; ///< evdev code
    bool pressed = false;
    uint32_t time_ms = 0;
};
// Express keys on the pad. `group` and `mode` are the mode group this button
// belongs to and the mode it is in now.
struct TabletPadButtonEvent {
    uint32_t device = 0;
    uint32_t button = 0; ///< index, from 0
    bool pressed = false;
    uint32_t group = 0, mode = 0;
    uint32_t time_ms = 0;
};
// A ring: `angle` in degrees clockwise from the top, or a negative value when
// the finger lifted.
struct TabletPadRingEvent {
    uint32_t device = 0;
    uint32_t ring = 0;
    double angle = -1.0;
    uint32_t time_ms = 0;
};
// A strip: `position` 0..1 along it, or a negative value when the finger lifted.
struct TabletPadStripEvent {
    uint32_t device = 0;
    uint32_t strip = 0;
    double position = -1.0;
    uint32_t time_ms = 0;
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
                                TouchFrameEvent, TouchCancelEvent, InputCapabilities,
                                TabletDeviceAdded, TabletDeviceRemoved, TabletToolProximityEvent,
                                TabletToolAxisEvent, TabletToolTipEvent, TabletToolButtonEvent,
                                TabletPadButtonEvent, TabletPadRingEvent, TabletPadStripEvent>;

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
