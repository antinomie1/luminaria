// luminaria/backend/libinput.cppm — bare-metal input via libinput. Emits keyboard,
// pointer and scroll events the compositor routes into a Seat.
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

export module luminaria.gpu:libinput;

import std;

import luminaria;
import :session;

export namespace luminaria {

class LibinputBackend {
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
    [[nodiscard]] bool set_keymap(const std::string& xkb_text);
    /// The layout in text form — by default the one XKB_DEFAULT_LAYOUT and
    /// friends select, which is what the user configured for the console.
    [[nodiscard]] const std::string& keymap() const noexcept;

    /// The backend's live keyboard state — kept up to date by every key event,
    /// so read keysyms and modifier masks off it rather than re-feeding the key
    /// signal into a second xkb state that could drift from the one clients are
    /// told about.
    [[nodiscard]] const KeymapState& keymap_state() const noexcept;

    /// What the currently open devices can do. Empty until `start()`, then kept
    /// up to date as devices come and go.
    [[nodiscard]] InputCapabilities capabilities() const noexcept;

    [[nodiscard]] Signal<KeyEvent>& key() noexcept;
    /// Modifier state after each key, emitted only when it actually changed.
    /// Feed to `Seat::notify_modifiers()`; without it clients see every key as
    /// unshifted and Ctrl-anything never reaches them as a shortcut.
    [[nodiscard]] Signal<ModifiersEvent>& modifiers() noexcept;
    [[nodiscard]] Signal<PointerMotionEvent>& pointer_motion() noexcept;
    [[nodiscard]] Signal<PointerButtonEvent>& pointer_button() noexcept;
    /// One scroll frame per libinput event: wheel notches in `d*_steps`, smooth
    /// touchpad deltas in `dx/dy`, end-of-gesture in `stop_*`.
    [[nodiscard]] Signal<PointerAxisEvent>& pointer_axis() noexcept;
    /// A device was added or removed and the aggregate capabilities changed.
    /// Re-advertise them on the seat here.
    [[nodiscard]] Signal<InputCapabilities>& capabilities_changed() noexcept;

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

    Signal<KeyEvent> key;
    Signal<ModifiersEvent> modifiers;
    Signal<PointerMotionEvent> pointer_motion;
    Signal<PointerButtonEvent> pointer_button;
    Signal<PointerAxisEvent> pointer_axis;
    Signal<InputCapabilities> capabilities_changed;

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
        modifiers.emit(now);
    }

    void device_added(libinput_device* device) {
        InputCapabilities c{
            libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD) != 0,
            libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER) != 0,
            libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TOUCH) != 0};
        devices[device] = c;
        recompute_capabilities();
    }

    void device_removed(libinput_device* device) {
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
        capabilities_changed.emit(now);
    }

    void handle_key(libinput_event* event) {
        auto* k = libinput_event_get_keyboard_event(event);
        const uint32_t code = libinput_event_keyboard_get_key(k);
        const bool pressed =
            libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED;
        KeyEvent e{code, pressed};
        key.emit(e);
        // After the key, the way wl_keyboard is specified: the mask applies to
        // what the user types NEXT, not to the modifier key itself.
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
            pointer_axis.emit(e);
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
                PointerMotionEvent e{libinput_event_pointer_get_dx(p),
                                     libinput_event_pointer_get_dy(p)};
                pointer_motion.emit(e);
                break;
            }
            case LIBINPUT_EVENT_POINTER_BUTTON: {
                auto* p = libinput_event_get_pointer_event(event);
                PointerButtonEvent e{libinput_event_pointer_get_button(p),
                                     libinput_event_pointer_get_button_state(p) ==
                                         LIBINPUT_BUTTON_STATE_PRESSED};
                pointer_button.emit(e);
                break;
            }
            case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
            case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
            case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
                handle_scroll(event, type);
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
    return true;
}

const std::string& LibinputBackend::keymap() const noexcept { return impl_->keymap.text(); }

const KeymapState& LibinputBackend::keymap_state() const noexcept { return impl_->keymap; }

InputCapabilities LibinputBackend::capabilities() const noexcept { return impl_->caps; }

Signal<KeyEvent>& LibinputBackend::key() noexcept { return impl_->key; }
Signal<ModifiersEvent>& LibinputBackend::modifiers() noexcept { return impl_->modifiers; }
Signal<PointerMotionEvent>& LibinputBackend::pointer_motion() noexcept {
    return impl_->pointer_motion;
}
Signal<PointerButtonEvent>& LibinputBackend::pointer_button() noexcept {
    return impl_->pointer_button;
}
Signal<PointerAxisEvent>& LibinputBackend::pointer_axis() noexcept { return impl_->pointer_axis; }
Signal<InputCapabilities>& LibinputBackend::capabilities_changed() noexcept {
    return impl_->capabilities_changed;
}

} // namespace luminaria
