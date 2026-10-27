// luminaria/backend/libinput.cppm — bare-metal input via libinput. Emits keyboard,
// pointer, scroll and touchscreen events the compositor routes into a Seat.
//
// Devices are opened through a luminaria::Session (libseat) when one is given,
// and directly otherwise — in which case it relies on the logind ACLs a
// logged-in VT gets. Codes are raw evdev (what wl_keyboard/wl_pointer expect),
// so they pass straight to Seat.
//
// The backend also owns an xkb state machine, because libinput reports keycodes
// and nothing else: whether Shift is down is a question only the keymap can
// answer. Hand `keymap()` to `Seat::set_keymap()` and the modifier masks emitted
// here are computed against the same layout the clients were told about. The
// machine itself is a `KeymapState` — the same object a compositor reads
// keysyms off, so bindings and masks can never disagree with what is sent.
//
// SAFETY: create() only builds the context; start() opens the devices (and thus
// grabs input). Never call start() under another compositor or you steal input.

module;

#include <cstdint>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <libinput.h>
#include <libudev.h>
#include <xkbcommon/xkbcommon.h>

export module luminaria.backend:libinput;

import std;

import luminaria;
import :session;

export namespace luminaria {

class LibinputBackend final : public InputSource {
public:
    /// Build the libinput/udev context. Does NOT open any input device yet.
    ///
    /// Pass a `Session` (luminaria/session.cppm) and devices are opened through
    /// libseat, so a VT switch revokes them cleanly and input is suspended
    /// while another session has the console. Without one it falls back to
    /// opening /dev/input/event* directly, which needs the logind ACLs of a
    /// logged-in VT and cannot survive a switch.
    [[nodiscard]] static Result<LibinputBackend> create(EventLoop loop, Session* session = nullptr);

    /// Build a context that opens exactly the devices you name with
    /// `add_device()` instead of everything udev reports on the seat. This is
    /// what makes libinput testable — a uinput device can be fed to it without
    /// a seat, a VT, or the user's real keyboard — and it is also the right
    /// choice for a compositor that keeps its own device list.
    [[nodiscard]] static Result<LibinputBackend> create_path(EventLoop loop,
                                                             Session* session = nullptr);

    ~LibinputBackend();
    LibinputBackend(LibinputBackend&&) noexcept;
    LibinputBackend& operator=(LibinputBackend&&) noexcept;
    LibinputBackend(const LibinputBackend&) = delete;
    LibinputBackend& operator=(const LibinputBackend&) = delete;

    /// Open one `/dev/input/eventN` on a `create_path()` context. GRABS THAT
    /// DEVICE immediately — the events start flowing once `start()` has hooked
    /// the context up to the event loop. Fails on a udev context, which chooses
    /// its own devices.
    Status add_device(const std::string& path);

    /// Assign the seat, open devices, and start delivering events. GRABS INPUT.
    /// On a `create_path()` context it opens nothing by itself; it only starts
    /// dispatching what `add_device()` gave it.
    Status start();

    /// Replace the keyboard layout the modifier masks are computed against.
    /// Returns false if the text doesn't compile, in which case the old layout
    /// stays. Pass the SAME text to `Seat::set_keymap()`: masks mean nothing
    /// without the keymap they were computed against.
    /// A layout that does compile is also announced as a `KeymapChange`.
    [[nodiscard]] bool set_keymap(const std::string& xkb_text);
    /// The layout in text form — by default the one XKB_DEFAULT_LAYOUT and
    /// friends select, which is what the user configured for the console.
    [[nodiscard]] const std::string& keymap() const noexcept override;

    /// The backend's live keyboard state — kept up to date by every key event,
    /// so read keysyms and modifier masks off it rather than re-feeding the key
    /// signal into a second xkb state that could drift from the one clients are
    /// told about.
    [[nodiscard]] const KeymapState& keymap_state() const noexcept;

    /// What the currently open devices can do. Empty until `start()`, then kept
    /// up to date as devices come and go.
    [[nodiscard]] InputCapabilities capabilities() const noexcept override;

    /// Relative motion, buttons, one `PointerAxisEvent` per libinput scroll
    /// event (wheel notches in `d*_steps` alongside their smooth value), keys,
    /// `ModifiersEvent` only when the masks actually changed, touchscreen
    /// points normalised to the panel (see `TouchDownEvent`), graphics-tablet
    /// devices, tools and pads (see `TabletDeviceAdded`; feed them to a
    /// `TabletManager`), and `InputCapabilities` as devices come and go.
    [[nodiscard]] Signal<InputEvent>& input() noexcept override;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit LibinputBackend(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct LibinputBackend::Impl {
    Impl(EventLoop loop_, Session* session_, KeymapState keymap_)
        : loop(loop_), session(session_), keymap(std::move(keymap_)) {}

    EventLoop loop;
    udev* udev_ctx = nullptr; // null on a path context
    libinput* li = nullptr;
    bool udev_seat = true;
    EventSource fd_source;

    // With a session, every /dev/input/event* goes through libseat so the
    // kernel can revoke it on a VT switch. The map is what close_restricted
    // needs to give the device back by id rather than by fd.
    Session* session = nullptr;
    std::map<int, int> session_devices; // fd -> libseat device id
    Signal<SessionActive>::Connection session_conn;

    // The keymap is ours, not libinput's: libinput reports keycodes and the
    // xkb state machine turns the modifier keys among them into masks. It is a
    // KeymapState — RAII, and shared with the compositor via keymap_state().
    KeymapState keymap;
    ModifiersEvent last_mods{};

    // Devices are counted rather than OR'd, so unplugging the mouse while a
    // touchpad stays behind does not drop the pointer capability.
    std::map<libinput_device*, InputCapabilities> devices;
    InputCapabilities caps;

    Signal<InputEvent> input;

    // Tablets and their pads get ids of our own: libinput_device pointers are
    // not something an event should carry, and a pointer can be reused.
    std::map<libinput_device*, std::uint32_t> tablet_ids;
    std::uint32_t next_tablet_id = 1;

    void emit(InputEvent event) { input.emit(event); }

    ~Impl() {
        fd_source = EventSource{};
        if (li != nullptr) {
            libinput_unref(li);
        }
        if (session != nullptr) {
            for (const auto& [fd, id] : session_devices) {
                session->close_device(id);
            }
            session_devices.clear();
        }
        if (udev_ctx != nullptr) {
            udev_unref(udev_ctx);
        }
    }

    void update_modifiers() {
        ModifiersEvent now = keymap.modifiers();
        if (now.depressed == last_mods.depressed && now.latched == last_mods.latched &&
            now.locked == last_mods.locked && now.group == last_mods.group) {
            return;
        }
        last_mods = now;
        emit(now);
    }

    void tablet_added(libinput_device* device) {
        const bool pad = libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TABLET_PAD) != 0;
        if (!pad && libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TABLET_TOOL) == 0) {
            return;
        }
        TabletDeviceAdded added;
        added.device = next_tablet_id++;
        added.name = libinput_device_get_name(device);
        added.vid = static_cast<std::uint16_t>(libinput_device_get_id_vendor(device));
        added.pid = static_cast<std::uint16_t>(libinput_device_get_id_product(device));
        added.path = std::string("/dev/input/") + libinput_device_get_sysname(device);
        added.is_pad = pad;
        if (pad) {
            TabletPadLayout& layout = added.pad;
            layout.buttons = static_cast<std::uint32_t>(
                std::max(0, libinput_device_tablet_pad_get_num_buttons(device)));
            layout.rings = static_cast<std::uint32_t>(
                std::max(0, libinput_device_tablet_pad_get_num_rings(device)));
            layout.strips = static_cast<std::uint32_t>(
                std::max(0, libinput_device_tablet_pad_get_num_strips(device)));
            const int groups = libinput_device_tablet_pad_get_num_mode_groups(device);
            for (int g = 0; g < groups; ++g) {
                libinput_tablet_pad_mode_group* group =
                    libinput_device_tablet_pad_get_mode_group(device, static_cast<unsigned>(g));
                if (group == nullptr) {
                    continue;
                }
                TabletPadGroup out;
                out.modes = static_cast<std::uint32_t>(
                    std::max(1u, libinput_tablet_pad_mode_group_get_num_modes(group)));
                for (std::uint32_t b = 0; b < layout.buttons; ++b) {
                    if (libinput_tablet_pad_mode_group_has_button(group, b) != 0) {
                        out.buttons.push_back(b);
                    }
                }
                for (std::uint32_t r = 0; r < layout.rings; ++r) {
                    if (libinput_tablet_pad_mode_group_has_ring(group, r) != 0) {
                        out.rings.push_back(r);
                    }
                }
                for (std::uint32_t t = 0; t < layout.strips; ++t) {
                    if (libinput_tablet_pad_mode_group_has_strip(group, t) != 0) {
                        out.strips.push_back(t);
                    }
                }
                layout.groups.push_back(std::move(out));
            }
        }
        tablet_ids[device] = added.device;
        emit(std::move(added));
    }

    void tablet_removed(libinput_device* device) {
        auto it = tablet_ids.find(device);
        if (it == tablet_ids.end()) {
            return;
        }
        const std::uint32_t id = it->second;
        tablet_ids.erase(it);
        emit(TabletDeviceRemoved{id});
    }

    static TabletToolType tool_type(libinput_tablet_tool* tool) {
        switch (libinput_tablet_tool_get_type(tool)) {
        case LIBINPUT_TABLET_TOOL_TYPE_ERASER:
            return TabletToolType::eraser;
        case LIBINPUT_TABLET_TOOL_TYPE_BRUSH:
            return TabletToolType::brush;
        case LIBINPUT_TABLET_TOOL_TYPE_PENCIL:
            return TabletToolType::pencil;
        case LIBINPUT_TABLET_TOOL_TYPE_AIRBRUSH:
            return TabletToolType::airbrush;
        case LIBINPUT_TABLET_TOOL_TYPE_MOUSE:
            return TabletToolType::mouse;
        case LIBINPUT_TABLET_TOOL_TYPE_LENS:
            return TabletToolType::lens;
        default:
            return TabletToolType::pen;
        }
    }

    /// What the tool in this event reports. `everything` on proximity-in, where
    /// the client needs the whole starting state; otherwise only what moved.
    static TabletAxes tool_axes(libinput_event_tablet_tool* e, bool everything) {
        libinput_tablet_tool* tool = libinput_event_tablet_tool_get_tool(e);
        TabletAxes a;
        if (everything || libinput_event_tablet_tool_x_has_changed(e) != 0 ||
            libinput_event_tablet_tool_y_has_changed(e) != 0) {
            a.changed |= TabletAxes::position;
            a.x = libinput_event_tablet_tool_get_x_transformed(e, 1);
            a.y = libinput_event_tablet_tool_get_y_transformed(e, 1);
        }
        if (libinput_tablet_tool_has_pressure(tool) != 0 &&
            (everything || libinput_event_tablet_tool_pressure_has_changed(e) != 0)) {
            a.changed |= TabletAxes::pressure_axis;
            a.pressure = libinput_event_tablet_tool_get_pressure(e);
        }
        if (libinput_tablet_tool_has_distance(tool) != 0 &&
            (everything || libinput_event_tablet_tool_distance_has_changed(e) != 0)) {
            a.changed |= TabletAxes::distance_axis;
            a.distance = libinput_event_tablet_tool_get_distance(e);
        }
        if (libinput_tablet_tool_has_tilt(tool) != 0 &&
            (everything || libinput_event_tablet_tool_tilt_x_has_changed(e) != 0 ||
             libinput_event_tablet_tool_tilt_y_has_changed(e) != 0)) {
            a.changed |= TabletAxes::tilt;
            a.tilt_x = libinput_event_tablet_tool_get_tilt_x(e);
            a.tilt_y = libinput_event_tablet_tool_get_tilt_y(e);
        }
        if (libinput_tablet_tool_has_rotation(tool) != 0 &&
            (everything || libinput_event_tablet_tool_rotation_has_changed(e) != 0)) {
            a.changed |= TabletAxes::rotation_axis;
            a.rotation = libinput_event_tablet_tool_get_rotation(e);
        }
        if (libinput_tablet_tool_has_slider(tool) != 0 &&
            (everything || libinput_event_tablet_tool_slider_has_changed(e) != 0)) {
            a.changed |= TabletAxes::slider_axis;
            a.slider = libinput_event_tablet_tool_get_slider_position(e);
        }
        // A wheel is a delta, so it only means something when it moved.
        if (libinput_tablet_tool_has_wheel(tool) != 0 &&
            libinput_event_tablet_tool_wheel_has_changed(e) != 0) {
            a.changed |= TabletAxes::wheel_axis;
            a.wheel_degrees = libinput_event_tablet_tool_get_wheel_delta(e);
            a.wheel_clicks =
                static_cast<std::int32_t>(libinput_event_tablet_tool_get_wheel_delta_discrete(e));
        }
        return a;
    }

    void handle_tablet_tool(libinput_event* event, libinput_event_type type) {
        auto* e = libinput_event_get_tablet_tool_event(event);
        auto* tool = libinput_event_tablet_tool_get_tool(e);
        auto it = tablet_ids.find(libinput_event_get_device(event));
        if (it == tablet_ids.end()) {
            return;
        }
        const std::uint32_t device = it->second;
        const auto tool_id = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(tool));
        const auto time = libinput_event_tablet_tool_get_time(e);
        switch (type) {
        case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY: {
            TabletToolProximityEvent p;
            p.device = device;
            p.tool = tool_id;
            p.in = libinput_event_tablet_tool_get_proximity_state(e) ==
                   LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_IN;
            p.type = tool_type(tool);
            p.serial = libinput_tablet_tool_get_serial(tool);
            p.wacom_id = libinput_tablet_tool_get_tool_id(tool);
            p.caps = TabletToolCaps{
                .tilt = libinput_tablet_tool_has_tilt(tool) != 0,
                .pressure = libinput_tablet_tool_has_pressure(tool) != 0,
                .distance = libinput_tablet_tool_has_distance(tool) != 0,
                .rotation = libinput_tablet_tool_has_rotation(tool) != 0,
                .slider = libinput_tablet_tool_has_slider(tool) != 0,
                .wheel = libinput_tablet_tool_has_wheel(tool) != 0};
            p.axes = tool_axes(e, p.in);
            p.time_ms = time;
            emit(std::move(p));
            break;
        }
        case LIBINPUT_EVENT_TABLET_TOOL_AXIS: {
            TabletToolAxisEvent a{device, tool_id, tool_axes(e, false), time};
            if (a.axes.changed != 0) {
                emit(std::move(a));
            }
            break;
        }
        case LIBINPUT_EVENT_TABLET_TOOL_TIP: {
            TabletToolTipEvent t{device, tool_id,
                                 libinput_event_tablet_tool_get_tip_state(e) ==
                                     LIBINPUT_TABLET_TOOL_TIP_DOWN,
                                 tool_axes(e, false), time};
            emit(std::move(t));
            break;
        }
        case LIBINPUT_EVENT_TABLET_TOOL_BUTTON: {
            emit(TabletToolButtonEvent{device, tool_id, libinput_event_tablet_tool_get_button(e),
                                       libinput_event_tablet_tool_get_button_state(e) ==
                                           LIBINPUT_BUTTON_STATE_PRESSED,
                                       time});
            break;
        }
        default:
            break;
        }
    }

    void handle_tablet_pad(libinput_event* event, libinput_event_type type) {
        auto* e = libinput_event_get_tablet_pad_event(event);
        auto it = tablet_ids.find(libinput_event_get_device(event));
        if (it == tablet_ids.end()) {
            return;
        }
        const std::uint32_t device = it->second;
        const auto time = libinput_event_tablet_pad_get_time(e);
        switch (type) {
        case LIBINPUT_EVENT_TABLET_PAD_BUTTON: {
            libinput_tablet_pad_mode_group* group = libinput_event_tablet_pad_get_mode_group(e);
            const std::uint32_t group_index =
                group != nullptr ? libinput_tablet_pad_mode_group_get_index(group) : 0;
            emit(TabletPadButtonEvent{device, libinput_event_tablet_pad_get_button_number(e),
                                      libinput_event_tablet_pad_get_button_state(e) ==
                                          LIBINPUT_BUTTON_STATE_PRESSED,
                                      group_index, libinput_event_tablet_pad_get_mode(e), time});
            break;
        }
        case LIBINPUT_EVENT_TABLET_PAD_RING:
            emit(TabletPadRingEvent{device, libinput_event_tablet_pad_get_ring_number(e),
                                    libinput_event_tablet_pad_get_ring_position(e), time});
            break;
        case LIBINPUT_EVENT_TABLET_PAD_STRIP:
            emit(TabletPadStripEvent{device, libinput_event_tablet_pad_get_strip_number(e),
                                     libinput_event_tablet_pad_get_strip_position(e), time});
            break;
        default:
            break;
        }
    }

    void device_added(libinput_device* device) {
        tablet_added(device);
        InputCapabilities c{
            libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD) != 0,
            libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER) != 0,
            libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TOUCH) != 0};
        devices[device] = c;
        recompute_capabilities();
    }

    void device_removed(libinput_device* device) {
        tablet_removed(device);
        devices.erase(device);
        recompute_capabilities();
    }

    void recompute_capabilities() {
        InputCapabilities now{};
        for (const auto& [device, c] : devices) {
            now.keyboard = now.keyboard || c.keyboard;
            now.pointer = now.pointer || c.pointer;
            now.touch = now.touch || c.touch;
        }
        if (now == caps) {
            return;
        }
        caps = now;
        emit(now);
    }

    void handle_key(libinput_event* event) {
        auto* k = libinput_event_get_keyboard_event(event);
        const uint32_t code = libinput_event_keyboard_get_key(k);
        const bool pressed =
            libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED;
        // Read before the key goes in, the way wl_keyboard is specified: the
        // mask applies to what the user types NEXT, not to the modifier key
        // itself.
        emit(KeyEvent{code, pressed, keymap.keysym(code), keymap.modifiers()});
        keymap.update_key(code, pressed);
        update_modifiers();
    }

    void handle_scroll(libinput_event* event, libinput_event_type type) {
        auto* p = libinput_event_get_pointer_event(event);
        const bool wheel = type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL;
        PointerAxisEvent e{};
        bool any = false;
        for (const auto axis : {LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL,
                                LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL}) {
            if (libinput_event_pointer_has_axis(p, axis) == 0) {
                continue;
            }
            const bool vertical = axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL;
            const double value = libinput_event_pointer_get_scroll_value(p, axis);
            if (vertical) {
                e.dy = value;
            } else {
                e.dx = value;
            }
            if (wheel) {
                // v120 is 120 per notch, so a hi-res wheel can report a
                // fraction of one. Sub-notch movement has no discrete value to
                // report and survives in the smooth delta above.
                const auto v120 =
                    static_cast<int32_t>(libinput_event_pointer_get_scroll_value_v120(p, axis));
                (vertical ? e.dy_steps : e.dx_steps) = v120 / 120;
            } else if (value == 0.0) {
                // A zero-valued finger/continuous scroll IS the end of the
                // gesture; that is how libinput says "fingers lifted".
                (vertical ? e.stop_vertical : e.stop_horizontal) = true;
            }
            any = true;
        }
        if (any) {
            emit(e);
        }
    }

    /// The seat slot, not the device slot: two touchscreens can both have a
    /// slot 0 down at once, and wl_touch ids are per seat. `_transformed` with
    /// a size of 1 is how libinput hands out a position normalised to the
    /// panel with its calibration matrix applied; any other size would claim
    /// to know which output the panel covers, which this backend does not.
    void handle_touch(libinput_event* event, libinput_event_type type) {
        auto* t = libinput_event_get_touch_event(event);
        const auto id = static_cast<int32_t>(libinput_event_touch_get_seat_slot(t));
        switch (type) {
        case LIBINPUT_EVENT_TOUCH_DOWN: {
            TouchDownEvent e{id, libinput_event_touch_get_x_transformed(t, 1),
                             libinput_event_touch_get_y_transformed(t, 1)};
            emit(e);
            break;
        }
        case LIBINPUT_EVENT_TOUCH_MOTION: {
            TouchMotionEvent e{id, libinput_event_touch_get_x_transformed(t, 1),
                               libinput_event_touch_get_y_transformed(t, 1)};
            emit(e);
            break;
        }
        case LIBINPUT_EVENT_TOUCH_UP: {
            TouchUpEvent e{id};
            emit(e);
            break;
        }
        case LIBINPUT_EVENT_TOUCH_CANCEL: {
            TouchCancelEvent e{id};
            emit(e);
            break;
        }
        case LIBINPUT_EVENT_TOUCH_FRAME: {
            TouchFrameEvent e{};
            emit(e);
            break;
        }
        default:
            break;
        }
    }

    void dispatch() {
        libinput_dispatch(li);
        libinput_event* event = nullptr;
        while ((event = libinput_get_event(li)) != nullptr) {
            const libinput_event_type type = libinput_event_get_type(event);
            switch (type) {
            case LIBINPUT_EVENT_DEVICE_ADDED:
                device_added(libinput_event_get_device(event));
                break;
            case LIBINPUT_EVENT_DEVICE_REMOVED:
                device_removed(libinput_event_get_device(event));
                break;
            case LIBINPUT_EVENT_KEYBOARD_KEY:
                handle_key(event);
                break;
            case LIBINPUT_EVENT_POINTER_MOTION: {
                auto* p = libinput_event_get_pointer_event(event);
                emit(PointerMotionEvent{libinput_event_pointer_get_dx(p),
                                        libinput_event_pointer_get_dy(p)});
                break;
            }
            case LIBINPUT_EVENT_POINTER_BUTTON: {
                auto* p = libinput_event_get_pointer_event(event);
                emit(PointerButtonEvent{libinput_event_pointer_get_button(p),
                                        libinput_event_pointer_get_button_state(p) ==
                                            LIBINPUT_BUTTON_STATE_PRESSED});
                break;
            }
            case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
            case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
            case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
                handle_scroll(event, type);
                break;
            case LIBINPUT_EVENT_TOUCH_DOWN:
            case LIBINPUT_EVENT_TOUCH_MOTION:
            case LIBINPUT_EVENT_TOUCH_UP:
            case LIBINPUT_EVENT_TOUCH_CANCEL:
            case LIBINPUT_EVENT_TOUCH_FRAME:
                handle_touch(event, type);
                break;
            case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY:
            case LIBINPUT_EVENT_TABLET_TOOL_AXIS:
            case LIBINPUT_EVENT_TABLET_TOOL_TIP:
            case LIBINPUT_EVENT_TABLET_TOOL_BUTTON:
                handle_tablet_tool(event, type);
                break;
            case LIBINPUT_EVENT_TABLET_PAD_BUTTON:
            case LIBINPUT_EVENT_TABLET_PAD_RING:
            case LIBINPUT_EVENT_TABLET_PAD_STRIP:
                handle_tablet_pad(event, type);
                break;
            default:
                break;
            }
            libinput_event_destroy(event);
        }
    }
};

namespace {

int open_restricted(const char* path, int flags, void* data) {
    auto* impl = static_cast<LibinputBackend::Impl*>(data);
    if (impl != nullptr && impl->session != nullptr) {
        int id = -1;
        if (auto fd = impl->session->open_device(path, id)) {
            impl->session_devices[*fd] = id;
            return *fd;
        }
        return -EACCES;
    }
    int fd = open(path, flags | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}
void close_restricted(int fd, void* data) {
    auto* impl = static_cast<LibinputBackend::Impl*>(data);
    if (impl != nullptr && impl->session != nullptr) {
        if (auto it = impl->session_devices.find(fd); it != impl->session_devices.end()) {
            impl->session->close_device(it->second);
            impl->session_devices.erase(it);
            return; // the session owns the fd
        }
    }
    close(fd);
}
constexpr libinput_interface kInterface{open_restricted, close_restricted};

/// The half of construction both context flavours share. The keymap is the
/// environment default — the layout the console is configured for — which is
/// what Seat::create() sends to clients, so the two agree until someone
/// changes either. Null means the keymap would not compile, the only way this
/// can fail.
std::unique_ptr<LibinputBackend::Impl> make_impl(EventLoop loop, Session* session) {
    auto keymap = KeymapState::from_layout();
    if (!keymap) {
        return nullptr;
    }
    return std::make_unique<LibinputBackend::Impl>(loop, session, std::move(*keymap));
}

} // namespace

LibinputBackend::LibinputBackend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LibinputBackend::~LibinputBackend() = default;
LibinputBackend::LibinputBackend(LibinputBackend&&) noexcept = default;
LibinputBackend& LibinputBackend::operator=(LibinputBackend&&) noexcept = default;

Result<LibinputBackend> LibinputBackend::create(EventLoop loop, Session* session) {
    std::unique_ptr<Impl> impl = make_impl(loop, session);
    if (!impl) {
        return fail("libinput: xkb keymap compilation failed");
    }
    udev* udev_ctx = udev_new();
    if (udev_ctx == nullptr) {
        return fail("libinput: udev_new failed");
    }
    impl->udev_ctx = udev_ctx;
    // The context's user data is the Impl, so open_restricted can reach the
    // session. It is created before the context for exactly that reason.
    libinput* li = libinput_udev_create_context(&kInterface, impl.get(), udev_ctx);
    if (li == nullptr) {
        udev_unref(udev_ctx);
        impl->udev_ctx = nullptr;
        return fail("libinput: create_context failed");
    }
    impl->li = li;
    impl->udev_seat = true;
    return LibinputBackend{std::move(impl)};
}

Result<LibinputBackend> LibinputBackend::create_path(EventLoop loop, Session* session) {
    std::unique_ptr<Impl> impl = make_impl(loop, session);
    if (!impl) {
        return fail("libinput: xkb keymap compilation failed");
    }
    libinput* li = libinput_path_create_context(&kInterface, impl.get());
    if (li == nullptr) {
        return fail("libinput: path_create_context failed");
    }
    impl->li = li;
    impl->udev_seat = false;
    return LibinputBackend{std::move(impl)};
}

Status LibinputBackend::add_device(const std::string& path) {
    if (impl_->udev_seat) {
        return fail("libinput: add_device needs a create_path() context");
    }
    if (libinput_path_add_device(impl_->li, path.c_str()) == nullptr) {
        return fail("libinput: cannot open " + path);
    }
    return ok();
}

Status LibinputBackend::start() {
    if (impl_->udev_seat && libinput_udev_assign_seat(impl_->li, "seat0") != 0) {
        return fail("libinput: assign_seat failed");
    }
    Impl* raw = impl_.get();
    impl_->fd_source = impl_->loop.add_fd(libinput_get_fd(impl_->li), [raw] { raw->dispatch(); });
    // libinput has to be told about the VT switch too, or it keeps reporting
    // keys pressed on someone else's session when we come back.
    if (impl_->session != nullptr) {
        impl_->session_conn =
            impl_->session->activity().connect([raw](SessionActive& event) {
                if (event.active) {
                    libinput_resume(raw->li);
                } else {
                    libinput_suspend(raw->li);
                }
            });
    }
    return ok();
}

bool LibinputBackend::set_keymap(const std::string& xkb_text) {
    if (xkb_text.empty() || xkb_text == impl_->keymap.text()) {
        return !xkb_text.empty();
    }
    auto fresh = KeymapState::from_text(xkb_text);
    if (!fresh) {
        return false; // the old layout stays
    }
    // The pressed-key state is deliberately NOT carried over: masks from the
    // old layout mean nothing under the new one, and clients get told the truth
    // on the next key.
    impl_->keymap = std::move(*fresh);
    impl_->emit(KeymapChange{impl_->keymap.text()});
    return true;
}

const std::string& LibinputBackend::keymap() const noexcept { return impl_->keymap.text(); }

const KeymapState& LibinputBackend::keymap_state() const noexcept { return impl_->keymap; }

InputCapabilities LibinputBackend::capabilities() const noexcept { return impl_->caps; }

Signal<InputEvent>& LibinputBackend::input() noexcept { return impl_->input; }

} // namespace luminaria
