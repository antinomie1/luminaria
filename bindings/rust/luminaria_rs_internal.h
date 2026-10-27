// luminaria_rs_internal.h — shared helpers and core shim accessors for the C++ shim TUs.
// Included AFTER `import luminaria;` in each shim .cpp file.
#pragma once

#include <wayland-server-core.h>

namespace lum {

namespace lm = luminaria;

template <class E>
using Conn = typename lm::Signal<E>::Connection;

// Client strings are not guaranteed UTF-8, and rust::String's checked
// constructor throws on bad input. Lossy never throws.
inline rust::String to_rust(const std::string& s) { return rust::String::lossy(s); }

inline rust::Vec<rust::String> to_rust(const std::vector<std::string>& v) {
    rust::Vec<rust::String> out;
    for (const auto& s : v) out.push_back(to_rust(s));
    return out;
}

inline Rect to_rect(const lm::Box& b) { return Rect{b.x, b.y, b.width, b.height}; }
inline lm::Box to_box(const Rect& r) { return lm::Box{r.x, r.y, r.width, r.height}; }

inline lm::SurfaceId id_from_u64(std::uint64_t raw) {
    if (raw == 0) return lm::SurfaceId{};
    return lm::SurfaceId{
        static_cast<std::uint32_t>(raw & 0xFFFFFFFFu),
        static_cast<std::uint32_t>(raw >> 32),
    };
}

inline std::uint64_t u64_from_id(lm::SurfaceId id) {
    return id.valid() ? ((static_cast<std::uint64_t>(id.generation) << 32) | id.index) : 0;
}

inline std::uint64_t surface_u64(lm::Surface* s) { return s != nullptr ? u64_from_id(s->id()) : 0; }

inline std::uint32_t caps_bits(lm::InputCapabilities c) {
    return (c.keyboard ? 1u : 0u) | (c.pointer ? 2u : 0u) | (c.touch ? 4u : 0u);
}

inline TabletAxesRaw tablet_axes_raw(const lm::TabletAxes& a) {
    TabletAxesRaw r{};
    r.changed = a.changed;
    r.x = a.x;
    r.y = a.y;
    r.pressure = a.pressure;
    r.distance = a.distance;
    r.tilt_x = a.tilt_x;
    r.tilt_y = a.tilt_y;
    r.rotation = a.rotation;
    r.slider = a.slider;
    r.wheel_degrees = a.wheel_degrees;
    r.wheel_clicks = a.wheel_clicks;
    return r;
}

// One `lm::InputEvent` for Rust, whichever source emitted it. Absolute motion
// leaves `output` 0: only the source knows which `OutputShim` id its output got.
inline InputEventRaw input_event_raw(const lm::InputEvent& event) {
    InputEventRaw r{};
    r.pressed = false;
    std::visit(
        [&r](const auto& e) {
            using E = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<E, lm::KeyEvent>) {
                r.kind = InputEventKind::Key;
                r.code = e.keycode;
                r.pressed = e.pressed;
                r.keysym = e.keysym;
                r.depressed = e.modifiers.depressed;
                r.latched = e.modifiers.latched;
                r.locked = e.modifiers.locked;
                r.group = e.modifiers.group;
            } else if constexpr (std::is_same_v<E, lm::ModifiersEvent>) {
                r.kind = InputEventKind::Modifiers;
                r.depressed = e.depressed;
                r.latched = e.latched;
                r.locked = e.locked;
                r.group = e.group;
            } else if constexpr (std::is_same_v<E, lm::KeymapChange>) {
                r.kind = InputEventKind::KeymapChange;
                r.text = rust::String::lossy(e.text.data(), e.text.size());
            } else if constexpr (std::is_same_v<E, lm::PointerMotionEvent>) {
                r.kind = InputEventKind::Motion;
                r.x = e.dx;
                r.y = e.dy;
            } else if constexpr (std::is_same_v<E, lm::PointerMotionAbsEvent>) {
                r.kind = InputEventKind::MotionAbsolute;
                r.x = e.x;
                r.y = e.y;
            } else if constexpr (std::is_same_v<E, lm::PointerLeaveEvent>) {
                r.kind = InputEventKind::PointerLeave;
            } else if constexpr (std::is_same_v<E, lm::PointerButtonEvent>) {
                r.kind = InputEventKind::Button;
                r.code = e.button;
                r.pressed = e.pressed;
            } else if constexpr (std::is_same_v<E, lm::PointerAxisEvent>) {
                r.kind = InputEventKind::Axis;
                r.x = e.dx;
                r.y = e.dy;
                r.steps_x = e.dx_steps;
                r.steps_y = e.dy_steps;
                r.flags = (e.stop_horizontal ? 1u : 0u) | (e.stop_vertical ? 2u : 0u);
            } else if constexpr (std::is_same_v<E, lm::TouchDownEvent>) {
                r.kind = InputEventKind::TouchDown;
                r.id = e.id;
                r.x = e.x;
                r.y = e.y;
            } else if constexpr (std::is_same_v<E, lm::TouchMotionEvent>) {
                r.kind = InputEventKind::TouchMotion;
                r.id = e.id;
                r.x = e.x;
                r.y = e.y;
            } else if constexpr (std::is_same_v<E, lm::TouchUpEvent>) {
                r.kind = InputEventKind::TouchUp;
                r.id = e.id;
            } else if constexpr (std::is_same_v<E, lm::TouchFrameEvent>) {
                r.kind = InputEventKind::TouchFrame;
            } else if constexpr (std::is_same_v<E, lm::TouchCancelEvent>) {
                r.kind = InputEventKind::TouchCancel;
                r.id = e.id;
            } else if constexpr (std::is_same_v<E, lm::TabletDeviceAdded>) {
                r.kind = InputEventKind::TabletDeviceAdded;
                r.device = e.device;
                r.text = to_rust(e.name);
                r.path = to_rust(e.path);
                r.vid = e.vid;
                r.pid = e.pid;
                r.bustype = e.bustype;
                r.flags = e.is_pad ? 1u : 0u;
                r.pad_buttons = e.pad.buttons;
                r.pad_rings = e.pad.rings;
                r.pad_strips = e.pad.strips;
                for (const lm::TabletPadGroup& g : e.pad.groups) {
                    PadGroupRaw out{};
                    out.modes = g.modes;
                    for (auto b : g.buttons) out.buttons.push_back(b);
                    for (auto ring : g.rings) out.rings.push_back(ring);
                    for (auto strip : g.strips) out.strips.push_back(strip);
                    r.groups.push_back(std::move(out));
                }
            } else if constexpr (std::is_same_v<E, lm::TabletDeviceRemoved>) {
                r.kind = InputEventKind::TabletDeviceRemoved;
                r.device = e.device;
            } else if constexpr (std::is_same_v<E, lm::TabletToolProximityEvent>) {
                r.kind = InputEventKind::TabletToolProximity;
                r.device = e.device;
                r.tool = e.tool;
                r.pressed = e.in;
                r.tool_type = static_cast<std::uint32_t>(e.type);
                r.serial = e.serial;
                r.wacom_id = e.wacom_id;
                r.caps = (e.caps.tilt ? 1u : 0u) | (e.caps.pressure ? 2u : 0u) |
                         (e.caps.distance ? 4u : 0u) | (e.caps.rotation ? 8u : 0u) |
                         (e.caps.slider ? 16u : 0u) | (e.caps.wheel ? 32u : 0u);
                r.axes = tablet_axes_raw(e.axes);
                r.time = e.time_ms;
            } else if constexpr (std::is_same_v<E, lm::TabletToolAxisEvent>) {
                r.kind = InputEventKind::TabletToolAxis;
                r.device = e.device;
                r.tool = e.tool;
                r.axes = tablet_axes_raw(e.axes);
                r.time = e.time_ms;
            } else if constexpr (std::is_same_v<E, lm::TabletToolTipEvent>) {
                r.kind = InputEventKind::TabletToolTip;
                r.device = e.device;
                r.tool = e.tool;
                r.pressed = e.down;
                r.axes = tablet_axes_raw(e.axes);
                r.time = e.time_ms;
            } else if constexpr (std::is_same_v<E, lm::TabletToolButtonEvent>) {
                r.kind = InputEventKind::TabletToolButton;
                r.device = e.device;
                r.tool = e.tool;
                r.code = e.button;
                r.pressed = e.pressed;
                r.time = e.time_ms;
            } else if constexpr (std::is_same_v<E, lm::TabletPadButtonEvent>) {
                r.kind = InputEventKind::TabletPadButton;
                r.device = e.device;
                r.code = e.button;
                r.pressed = e.pressed;
                r.group = e.group;
                r.mode = e.mode;
                r.time = e.time_ms;
            } else if constexpr (std::is_same_v<E, lm::TabletPadRingEvent>) {
                r.kind = InputEventKind::TabletPadRing;
                r.device = e.device;
                r.code = e.ring;
                r.x = e.angle;
                r.time = e.time_ms;
            } else if constexpr (std::is_same_v<E, lm::TabletPadStripEvent>) {
                r.kind = InputEventKind::TabletPadStrip;
                r.device = e.device;
                r.code = e.strip;
                r.x = e.position;
                r.time = e.time_ms;
            } else {
                static_assert(std::is_same_v<E, lm::InputCapabilities>);
                r.kind = InputEventKind::Capabilities;
                r.flags = caps_bits(e);
            }
        },
        event);
    return r;
}

// An event for Rust; `kind` says which of the other fields mean something.
inline EventRaw event(EventKind kind, std::uint64_t id = 0, std::uint64_t other = 0) {
    EventRaw e{};
    e.kind = kind;
    e.id = id;
    e.other = other;
    e.flag = false;
    e.flag2 = false;
    return e;
}

// Everything any shim forwards, in the order the signals fired, until
// `DisplayShim::next_event` hands it to Rust as one stream. C++ never calls
// Rust, so a signal can only leave something behind to be collected; keeping
// one queue rather than one per shim is what keeps the order and makes a queue
// nobody drains impossible. Per thread, because a Display and everything on it
// live on the thread that made them: no Rust wrapper is Send.
struct HubRecord {
    enum class Type : std::uint32_t { none = 0, raw = 1, input = 2, output_added = 3 };
    Type type = Type::none;
    EventRaw event{};
    InputEventRaw input{};
    std::unique_ptr<OutputShim> output; // output_added: its id is event.id
};
std::deque<HubRecord>& hub();

inline void hub_push(EventRaw e) {
    hub().push_back(HubRecord{HubRecord::Type::raw, std::move(e), {}, nullptr});
}
inline void hub_push_input(InputEventRaw e) {
    hub().push_back(HubRecord{HubRecord::Type::input, {}, std::move(e), nullptr});
}
void hub_push_output(std::unique_ptr<OutputShim> output);

// What a shim keeps to forward its signals.
struct Events {
    void push(EventRaw e) { hub_push(std::move(e)); }
};

struct StoredImage {
    int width = 0;
    int height = 0;
    std::vector<lm::Pixel> pixels;
};

// The popup behind a client's xdg_popup, and the layer surface behind an
// ours; both `Impl`s are private to the core TU.
lm::Popup* xdg_popup(const XdgShellShim& shell, std::uint64_t popup);
lm::LayerSurface* layer_surface(const LayerShellShim& shell, std::uint64_t surface_id);

// Identity for an object that has no surface of its own to be named by (a
// lock, an idle notification, a text input). Never reused.
std::uint64_t next_object_id();

// Whether `resource` (a client's wl_output) is `global`'s.
bool is_output(wl_resource* resource, const OutputGlobalShim& global);

// Cross-TU accessors for core shims:
lm::Display* raw_ptr(const DisplayShim& s);
lm::Output* raw_ptr(const OutputShim& s);
std::unique_ptr<OutputShim> make_output_shim(lm::Output* output);
lm::OutputLayout& raw_ref(const OutputLayoutShim& s);
lm::Seat* raw_ptr(const SeatShim& s);
lm::XdgShell* raw_ptr(const XdgShellShim& s);
lm::CursorShapeManager* raw_ptr(const CursorShapeManagerShim& s);
lm::DataDeviceManager* raw_ptr(const DataDeviceManagerShim& s);
lm::TextInputManager* raw_ptr(const TextInputManagerShim& s);

struct PresentationObserver {
    virtual ~PresentationObserver() = default;
    virtual void on_presentation_destroyed() = 0;
};

struct PresentationShim::Impl {
    std::optional<lm::Presentation> mgr;
    std::vector<PresentationObserver*> observers;

    // Before `mgr` goes: no renderer may still be pointing at it.
    ~Impl() {
        for (PresentationObserver* obs : observers) {
            obs->on_presentation_destroyed();
        }
    }
};

} // namespace lum
