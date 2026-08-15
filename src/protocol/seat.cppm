// luminaria/seat.cppm — the wl_seat: keyboard + pointer + touch input routing.
//
// The seat advertises input capabilities and routes events to the focused
// client. Keyboard focus is surface-scoped: keys go to whoever holds focus.
// Pointer focus follows the cursor. The compositor decides focus (from scene
// hit-testing) and calls the notify_*/pointer_* methods; the seat handles the
// wire events.
//
// Retained focus and cursor state is represented by SurfaceId. Every event
// resolves the id immediately before touching the surface, so client teardown
// cannot leave a dangling pointer in the input path.

module;

#include <cstdint>
#include <functional>
#include <typeinfo>
#include <memory>
#include <string>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

export module luminaria:seat;

import :compositor;
import :display;
import :expected;
import :signal;

export namespace luminaria {

class Display;
class Surface;

/// Keyboard focus moved (an invalid id means no surface).
struct SeatKeyboardFocus {
    SurfaceId surface;
};
/// Pointer focus moved (an invalid id means no surface).
struct SeatPointerFocus {
    SurfaceId surface;
};
/// The pointer-focused client set its cursor image.
///
/// The two ways of having no surface are NOT the same and a compositor has to
/// act on both: `hidden` means the client asked for no pointer at all (a video
/// player going fullscreen, an editor hiding it while you type) and the
/// compositor must draw nothing, while an invalid `surface` with `hidden` false
/// means the client has not said anything and the compositor's own cursor
/// applies.
struct SeatCursorChange {
    SurfaceId surface;
    int hotspot_x;
    int hotspot_y;
    bool hidden;
};

/// Callbacks a drag-and-drop implementation (wl_data_device) installs on the
/// seat. While a drag is active the seat sends no wl_pointer events to clients;
/// it drives these instead, as the protocol requires.
struct SeatDragHooks {
    /// Pointer moved onto `surface` (invalid = left every surface).
    std::function<void(SurfaceId surface, double sx, double sy)> focus;
    std::function<void(double sx, double sy)> motion;
    /// Button released — the drop happens (or is cancelled) here.
    std::function<void()> drop;
};

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

    /// Re-advertise capabilities to every bound client. Defaults to keyboard +
    /// pointer; enable touch once a touch device actually exists.
    void set_capabilities(bool keyboard, bool pointer, bool touch);

    /// Replace the keyboard layout with an xkb keymap in text form, and re-send
    /// it to every bound wl_keyboard. Returns false if it doesn't compile, in
    /// which case the old one stays.
    ///
    /// This is what keeps a NESTED compositor's keys honest: the parent reports
    /// modifier masks computed against ITS keymap, and interpreting those
    /// against a locally-guessed one only works by luck on a US layout. Adopt
    /// `WaylandBackend::keymap()` and the two agree by construction.
    [[nodiscard]] bool set_keymap(const std::string& xkb_text);
    [[nodiscard]] const std::string& keymap() const noexcept;

    // --- keyboard ---
    /// Give keyboard focus to `surface` (nullptr clears focus). Sends leave/enter.
    void set_keyboard_focus(Surface* surface);
    [[nodiscard]] SurfaceId keyboard_focus() const noexcept;
    /// Send a key event to the keyboard-focused client. `pressed` = down.
    void notify_key(uint32_t key, bool pressed);
    /// Send the current modifier state (Shift/Ctrl/…) to the keyboard-focused client.
    void notify_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
    [[nodiscard]] Signal<SeatKeyboardFocus>& keyboard_focus_changed() noexcept;

    // --- pointer ---
    /// Move pointer focus onto `surface` at local (sx,sy) and send enter.
    void pointer_enter(Surface& surface, double sx, double sy);
    /// Drop pointer focus entirely (cursor left every surface): sends leave.
    void pointer_clear_focus();
    [[nodiscard]] SurfaceId pointer_focus() const noexcept;
    /// Send motion to the pointer-focused client.
    void pointer_motion(double sx, double sy);
    /// Send a button event to the pointer-focused client. `pressed` = down.
    void pointer_button(uint32_t button, bool pressed);
    /// Smooth (touchpad / high-resolution wheel) scrolling, in surface units.
    /// Positive dy scrolls down, positive dx scrolls right.
    void pointer_axis(double dx, double dy);
    /// Notched wheel scrolling: `steps` clicks along each axis.
    void pointer_axis_discrete(int32_t dx_steps, int32_t dy_steps);
    /// Scrolling finished (fingers lifted). Ends a smooth-scroll sequence.
    void pointer_axis_stop(bool horizontal, bool vertical);

    /// Fires when the focused client sets (or hides) its cursor image.
    [[nodiscard]] Signal<SeatCursorChange>& cursor_changed() noexcept;
    /// Current client cursor surface (invalid = hidden, or never set).
    [[nodiscard]] SurfaceId cursor_surface() const noexcept;
    /// The focused client asked for NO cursor. Draw nothing — not the theme's
    /// arrow, which is what "never set" means.
    [[nodiscard]] bool cursor_hidden() const noexcept;
    [[nodiscard]] int cursor_hotspot_x() const noexcept;
    [[nodiscard]] int cursor_hotspot_y() const noexcept;
    [[nodiscard]] Signal<SeatPointerFocus>& pointer_focus_changed() noexcept;

    // --- touch ---
    /// Begin a touch point on `surface` at surface-local (x,y). `id` is the slot.
    void touch_down(Surface& surface, int32_t id, double x, double y);
    void touch_motion(int32_t id, double x, double y);
    void touch_up(int32_t id);
    /// End the current set of touch events (send once per input frame).
    void touch_frame();
    /// The compositor took over the gesture; clients must forget it.
    void touch_cancel();

    // --- drag and drop (installed by wl_data_device) ---
    void begin_drag(SeatDragHooks hooks);
    void end_drag();
    [[nodiscard]] bool dragging() const noexcept;

    struct Impl; // named by the protocol glue

private:
    std::unique_ptr<Impl> impl_;
    explicit Seat(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct Seat::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    std::string name;
    std::string keymap; // xkb keymap text (null-terminated payload sent to clients)

    std::vector<wl_resource*> seats; // bound wl_seat resources (for capability updates)
    std::vector<wl_resource*> keyboards;
    std::vector<wl_resource*> pointers;
    std::vector<wl_resource*> touches;
    uint32_t capabilities = WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER;

    SurfaceId kb_focus;
    SurfaceId ptr_focus;
    SurfaceId touch_focus;
    SurfaceId cursor;
    Signal<SurfaceInvalidated>::Connection invalidated;
    int cursor_hotspot_x = 0;
    int cursor_hotspot_y = 0;
    bool cursor_hidden = false;

    /// Back to "the compositor's own cursor applies" — the client that had an
    /// opinion is no longer the focused one.
    void forget_cursor() {
        if (!cursor.valid() && !cursor_hidden) {
            return;
        }
        cursor = {};
        cursor_hotspot_x = 0;
        cursor_hotspot_y = 0;
        cursor_hidden = false;
        SeatCursorChange gone{};
        cursor_changed.emit(gone);
    }

    Signal<SeatKeyboardFocus> keyboard_focus_changed;
    Signal<SeatPointerFocus> pointer_focus_changed;
    Signal<SeatCursorChange> cursor_changed;

    SeatDragHooks drag;
    bool dragging = false;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }

    [[nodiscard]] uint32_t next_serial() const { return wl_display_next_serial(display); }
};

namespace {

uint32_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

wl_client* client_of(Surface& surface) {
    return wl_resource_get_client(surface.c_resource());
}

void send_keymap(Seat::Impl* seat, wl_resource* keyboard) {
    const size_t size = seat->keymap.size() + 1; // include terminating NUL
    int fd = memfd_create("luminaria-keymap", MFD_CLOEXEC);
    if (fd < 0) {
        return;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) == 0) {
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) {
            std::memcpy(p, seat->keymap.c_str(), size);
            munmap(p, size);
            wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                    static_cast<uint32_t>(size));
        }
    }
    close(fd);
}

// ---- wl_keyboard / wl_pointer / wl_touch resource lifecycle ----
void resource_release(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
constexpr struct wl_keyboard_interface keyboard_impl = {.release = resource_release};

void keyboard_destroy(wl_resource* resource) {
    auto* seat = static_cast<Seat::Impl*>(wl_resource_get_user_data(resource));
    std::erase(seat->keyboards, resource);
}

// wl_pointer.set_cursor: the focused client hands us the surface to draw as the
// cursor (null = hide it). We just publish it; the compositor composites it.
void pointer_set_cursor(wl_client* client, wl_resource* resource, uint32_t /*serial*/,
                        wl_resource* surface_resource, int32_t hotspot_x, int32_t hotspot_y) {
    auto* seat = static_cast<Seat::Impl*>(wl_resource_get_user_data(resource));
    // Only the client that currently holds pointer focus may set the cursor.
    Surface* focus = surface_from_id(seat->ptr_focus);
    if (focus == nullptr || client_of(*focus) != client) {
        return;
    }
    auto* surface = surface_resource != nullptr
                        ? static_cast<Surface*>(wl_resource_get_user_data(surface_resource))
                        : nullptr;
    if (surface != nullptr) {
        // The cursor rides the pointer, so a hit test at the pointer would
        // otherwise find the cursor itself instead of the window under it.
        surface->set_input_transparent();
    }
    seat->cursor = surface != nullptr ? surface->id() : SurfaceId{};
    seat->cursor_hotspot_x = hotspot_x;
    seat->cursor_hotspot_y = hotspot_y;
    // A null surface is the client saying "no pointer", which is not the same
    // as never having said anything.
    seat->cursor_hidden = surface == nullptr;
    SeatCursorChange event{seat->cursor, hotspot_x, hotspot_y, seat->cursor_hidden};
    seat->cursor_changed.emit(event);
}
constexpr struct wl_pointer_interface pointer_impl = {.set_cursor = pointer_set_cursor,
                                                      .release = resource_release};
void pointer_destroy(wl_resource* resource) {
    auto* seat = static_cast<Seat::Impl*>(wl_resource_get_user_data(resource));
    std::erase(seat->pointers, resource);
}

constexpr struct wl_touch_interface touch_impl = {.release = resource_release};
void touch_destroy(wl_resource* resource) {
    auto* seat = static_cast<Seat::Impl*>(wl_resource_get_user_data(resource));
    std::erase(seat->touches, resource);
}

// ---- wl_seat requests ----
void seat_get_pointer(wl_client* client, wl_resource* seat_resource, uint32_t id) {
    auto* seat = static_cast<Seat::Impl*>(wl_resource_get_user_data(seat_resource));
    wl_resource* resource = wl_resource_create(client, &wl_pointer_interface,
                                               wl_resource_get_version(seat_resource), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &pointer_impl, seat, pointer_destroy);
    seat->pointers.push_back(resource);
}
void seat_get_keyboard(wl_client* client, wl_resource* seat_resource, uint32_t id) {
    auto* seat = static_cast<Seat::Impl*>(wl_resource_get_user_data(seat_resource));
    wl_resource* resource = wl_resource_create(client, &wl_keyboard_interface,
                                               wl_resource_get_version(seat_resource), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &keyboard_impl, seat, keyboard_destroy);
    seat->keyboards.push_back(resource);
    send_keymap(seat, resource);
    if (wl_resource_get_version(resource) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) {
        wl_keyboard_send_repeat_info(resource, 25, 600);
    }
}
void seat_get_touch(wl_client* client, wl_resource* seat_resource, uint32_t id) {
    auto* seat = static_cast<Seat::Impl*>(wl_resource_get_user_data(seat_resource));
    wl_resource* resource = wl_resource_create(client, &wl_touch_interface,
                                               wl_resource_get_version(seat_resource), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &touch_impl, seat, touch_destroy);
    seat->touches.push_back(resource);
}
void seat_release(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
constexpr struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

void seat_resource_destroy(wl_resource* resource) {
    auto* seat = static_cast<Seat::Impl*>(wl_resource_get_user_data(resource));
    std::erase(seat->seats, resource);
}

void seat_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* seat = static_cast<Seat::Impl*>(data);
    wl_resource* resource =
        wl_resource_create(client, &wl_seat_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &seat_impl, seat, seat_resource_destroy);
    seat->seats.push_back(resource);
    wl_seat_send_capabilities(resource, seat->capabilities);
    if (version >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(resource, seat->name.c_str());
    }
}

void pointer_frame_if_supported(wl_resource* pointer) {
    if (wl_resource_get_version(pointer) >= WL_POINTER_FRAME_SINCE_VERSION) {
        wl_pointer_send_frame(pointer);
    }
}

} // namespace

Seat::Seat(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Seat::~Seat() = default;
Seat::Seat(Seat&&) noexcept = default;
Seat& Seat::operator=(Seat&&) noexcept = default;

Result<Seat> Seat::create(Display& display, std::string name) {
    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (ctx == nullptr) {
        return fail("xkb_context_new failed");
    }
    xkb_keymap* keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap == nullptr) {
        xkb_context_unref(ctx);
        return fail("xkb_keymap_new_from_names failed");
    }
    char* keymap_str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    std::string keymap_string = keymap_str != nullptr ? keymap_str : "";
    free(keymap_str);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
    if (keymap_string.empty()) {
        return fail("xkb keymap serialization failed");
    }

    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->name = std::move(name);
    impl->keymap = std::move(keymap_string);
    impl->global = wl_global_create(impl->display, &wl_seat_interface, 5, impl.get(), seat_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wl_seat) failed");
    }
    Impl* raw = impl.get();
    impl->invalidated = surface_invalidated().connect([raw](SurfaceInvalidated& event) {
        if (raw->kb_focus == event.surface) {
            raw->kb_focus = {};
            SeatKeyboardFocus changed{};
            raw->keyboard_focus_changed.emit(changed);
        }

        const bool pointer_lost = raw->ptr_focus == event.surface;
        if (pointer_lost) {
            raw->ptr_focus = {};
            if (!raw->dragging) {
                SeatPointerFocus changed{};
                raw->pointer_focus_changed.emit(changed);
            }
        }

        if (raw->touch_focus == event.surface) {
            raw->touch_focus = {};
        }

        if (raw->cursor == event.surface || pointer_lost) {
            raw->forget_cursor();
        }
    });
    return Seat{std::move(impl)};
}

bool Seat::set_keymap(const std::string& xkb_text) {
    if (xkb_text.empty() || xkb_text == impl_->keymap) {
        return !xkb_text.empty();
    }
    // Compile it before believing it: a keymap we can't parse would leave every
    // client with a keyboard that types nothing.
    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (ctx == nullptr) {
        return false;
    }
    xkb_keymap* keymap = xkb_keymap_new_from_string(ctx, xkb_text.c_str(),
                                                    XKB_KEYMAP_FORMAT_TEXT_V1,
                                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
    const bool valid = keymap != nullptr;
    if (keymap != nullptr) {
        xkb_keymap_unref(keymap);
    }
    xkb_context_unref(ctx);
    if (!valid) {
        return false;
    }
    impl_->keymap = xkb_text;
    for (wl_resource* kb : impl_->keyboards) {
        send_keymap(impl_.get(), kb);
    }
    return true;
}

const std::string& Seat::keymap() const noexcept { return impl_->keymap; }

void Seat::set_capabilities(bool keyboard, bool pointer, bool touch) {
    uint32_t caps = 0;
    if (keyboard) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    if (pointer) {
        caps |= WL_SEAT_CAPABILITY_POINTER;
    }
    if (touch) {
        caps |= WL_SEAT_CAPABILITY_TOUCH;
    }
    if (caps == impl_->capabilities) {
        return;
    }
    impl_->capabilities = caps;
    for (wl_resource* seat : impl_->seats) {
        wl_seat_send_capabilities(seat, caps);
    }
}

// --- keyboard ---

void Seat::set_keyboard_focus(Surface* surface) {
    const SurfaceId next = surface != nullptr ? surface->id() : SurfaceId{};
    if (impl_->kb_focus == next) {
        return;
    }
    if (Surface* previous = surface_from_id(impl_->kb_focus); previous != nullptr) {
        wl_resource* old = previous->c_resource();
        wl_client* old_client = wl_resource_get_client(old);
        const uint32_t serial = impl_->next_serial();
        for (wl_resource* kb : impl_->keyboards) {
            if (wl_resource_get_client(kb) == old_client) {
                wl_keyboard_send_leave(kb, serial, old);
            }
        }
    }
    impl_->kb_focus = next;
    if (surface != nullptr) {
        wl_resource* res = surface->c_resource();
        wl_client* client = wl_resource_get_client(res);
        wl_array keys;
        wl_array_init(&keys);
        const uint32_t serial = impl_->next_serial();
        for (wl_resource* kb : impl_->keyboards) {
            if (wl_resource_get_client(kb) == client) {
                wl_keyboard_send_enter(kb, serial, res, &keys);
                wl_keyboard_send_modifiers(kb, impl_->next_serial(), 0, 0, 0, 0);
            }
        }
        wl_array_release(&keys);
    }
    SeatKeyboardFocus event{next};
    impl_->keyboard_focus_changed.emit(event);
}

SurfaceId Seat::keyboard_focus() const noexcept {
    return impl_->kb_focus;
}

Signal<SeatKeyboardFocus>& Seat::keyboard_focus_changed() noexcept {
    return impl_->keyboard_focus_changed;
}

void Seat::notify_key(uint32_t key, bool pressed) {
    Surface* focus = surface_from_id(impl_->kb_focus);
    if (focus == nullptr) {
        return;
    }
    wl_client* client = client_of(*focus);
    const uint32_t serial = impl_->next_serial();
    const uint32_t state =
        pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
    for (wl_resource* kb : impl_->keyboards) {
        if (wl_resource_get_client(kb) == client) {
            wl_keyboard_send_key(kb, serial, now_ms(), key, state);
        }
    }
}

void Seat::notify_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    Surface* focus = surface_from_id(impl_->kb_focus);
    if (focus == nullptr) {
        return;
    }
    wl_client* client = client_of(*focus);
    const uint32_t serial = impl_->next_serial();
    for (wl_resource* kb : impl_->keyboards) {
        if (wl_resource_get_client(kb) == client) {
            wl_keyboard_send_modifiers(kb, serial, depressed, latched, locked, group);
        }
    }
}

// --- pointer ---

void Seat::pointer_enter(Surface& surface, double sx, double sy) {
    const SurfaceId target = surface.id();
    if (impl_->dragging) {
        // During a drag the pointer belongs to the drag: no wl_pointer events.
        if (impl_->ptr_focus != target) {
            impl_->ptr_focus = target;
            if (impl_->drag.focus) {
                impl_->drag.focus(target, sx, sy);
            }
        } else if (impl_->drag.motion) {
            impl_->drag.motion(sx, sy);
        }
        return;
    }
    if (impl_->ptr_focus == target) {
        pointer_motion(sx, sy);
        return;
    }
    pointer_clear_focus();

    impl_->ptr_focus = target;

    wl_resource* res = surface.c_resource();
    wl_client* client = wl_resource_get_client(res);
    const uint32_t serial = impl_->next_serial();
    for (wl_resource* p : impl_->pointers) {
        if (wl_resource_get_client(p) == client) {
            wl_pointer_send_enter(p, serial, res, wl_fixed_from_double(sx),
                                  wl_fixed_from_double(sy));
            pointer_frame_if_supported(p);
        }
    }
    SeatPointerFocus event{target};
    impl_->pointer_focus_changed.emit(event);
}

void Seat::pointer_clear_focus() {
    if (!impl_->ptr_focus.valid()) {
        return;
    }
    if (impl_->dragging) {
        impl_->ptr_focus = {};
        if (impl_->drag.focus) {
            impl_->drag.focus({}, 0, 0);
        }
        return;
    }
    if (Surface* focus = surface_from_id(impl_->ptr_focus); focus != nullptr) {
        wl_resource* res = focus->c_resource();
        wl_client* client = wl_resource_get_client(res);
        const uint32_t serial = impl_->next_serial();
        for (wl_resource* p : impl_->pointers) {
            if (wl_resource_get_client(p) == client) {
                wl_pointer_send_leave(p, serial, res);
                pointer_frame_if_supported(p);
            }
        }
    }
    impl_->ptr_focus = {};
    // The cursor image belonged to that client, and so did its decision to hide
    // the pointer: neither survives the focus leaving.
    impl_->forget_cursor();
    SeatPointerFocus event{};
    impl_->pointer_focus_changed.emit(event);
}

SurfaceId Seat::pointer_focus() const noexcept {
    return impl_->ptr_focus;
}

Signal<SeatPointerFocus>& Seat::pointer_focus_changed() noexcept {
    return impl_->pointer_focus_changed;
}

void Seat::pointer_motion(double sx, double sy) {
    Surface* focus = surface_from_id(impl_->ptr_focus);
    if (focus == nullptr) {
        return;
    }
    if (impl_->dragging) {
        if (impl_->drag.motion) {
            impl_->drag.motion(sx, sy);
        }
        return;
    }
    wl_client* client = client_of(*focus);
    for (wl_resource* p : impl_->pointers) {
        if (wl_resource_get_client(p) == client) {
            wl_pointer_send_motion(p, now_ms(), wl_fixed_from_double(sx),
                                   wl_fixed_from_double(sy));
            pointer_frame_if_supported(p);
        }
    }
}

void Seat::pointer_button(uint32_t button, bool pressed) {
    if (impl_->dragging) {
        if (!pressed && impl_->drag.drop) {
            impl_->drag.drop();
        }
        return;
    }
    Surface* focus = surface_from_id(impl_->ptr_focus);
    if (focus == nullptr) {
        return;
    }
    wl_client* client = client_of(*focus);
    const uint32_t serial = impl_->next_serial();
    const uint32_t state =
        pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
    for (wl_resource* p : impl_->pointers) {
        if (wl_resource_get_client(p) == client) {
            wl_pointer_send_button(p, serial, now_ms(), button, state);
            pointer_frame_if_supported(p);
        }
    }
}

void Seat::pointer_axis(double dx, double dy) {
    Surface* focus = surface_from_id(impl_->ptr_focus);
    if (focus == nullptr || impl_->dragging || (dx == 0.0 && dy == 0.0)) {
        return;
    }
    wl_client* client = client_of(*focus);
    const uint32_t time = now_ms();
    for (wl_resource* p : impl_->pointers) {
        if (wl_resource_get_client(p) != client) {
            continue;
        }
        if (wl_resource_get_version(p) >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION) {
            wl_pointer_send_axis_source(p, WL_POINTER_AXIS_SOURCE_FINGER);
        }
        if (dy != 0.0) {
            wl_pointer_send_axis(p, time, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                 wl_fixed_from_double(dy));
        }
        if (dx != 0.0) {
            wl_pointer_send_axis(p, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                 wl_fixed_from_double(dx));
        }
        pointer_frame_if_supported(p);
    }
}

void Seat::pointer_axis_discrete(int32_t dx_steps, int32_t dy_steps) {
    Surface* focus = surface_from_id(impl_->ptr_focus);
    if (focus == nullptr || impl_->dragging || (dx_steps == 0 && dy_steps == 0)) {
        return;
    }
    // One wheel notch is 10 surface units by convention (what GTK/Qt expect).
    constexpr double kStep = 10.0;
    wl_client* client = client_of(*focus);
    const uint32_t time = now_ms();
    for (wl_resource* p : impl_->pointers) {
        if (wl_resource_get_client(p) != client) {
            continue;
        }
        const bool v5 = wl_resource_get_version(p) >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION;
        if (v5) {
            wl_pointer_send_axis_source(p, WL_POINTER_AXIS_SOURCE_WHEEL);
        }
        if (dy_steps != 0) {
            if (v5) {
                wl_pointer_send_axis_discrete(p, WL_POINTER_AXIS_VERTICAL_SCROLL, dy_steps);
            }
            wl_pointer_send_axis(p, time, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                 wl_fixed_from_double(dy_steps * kStep));
        }
        if (dx_steps != 0) {
            if (v5) {
                wl_pointer_send_axis_discrete(p, WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx_steps);
            }
            wl_pointer_send_axis(p, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                 wl_fixed_from_double(dx_steps * kStep));
        }
        pointer_frame_if_supported(p);
    }
}

void Seat::pointer_axis_stop(bool horizontal, bool vertical) {
    Surface* focus = surface_from_id(impl_->ptr_focus);
    if (focus == nullptr || impl_->dragging || (!horizontal && !vertical)) {
        return;
    }
    wl_client* client = client_of(*focus);
    const uint32_t time = now_ms();
    for (wl_resource* p : impl_->pointers) {
        if (wl_resource_get_client(p) != client ||
            wl_resource_get_version(p) < WL_POINTER_AXIS_STOP_SINCE_VERSION) {
            continue;
        }
        if (vertical) {
            wl_pointer_send_axis_stop(p, time, WL_POINTER_AXIS_VERTICAL_SCROLL);
        }
        if (horizontal) {
            wl_pointer_send_axis_stop(p, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
        }
        pointer_frame_if_supported(p);
    }
}

Signal<SeatCursorChange>& Seat::cursor_changed() noexcept {
    return impl_->cursor_changed;
}
bool Seat::cursor_hidden() const noexcept {
    return impl_->cursor_hidden;
}
SurfaceId Seat::cursor_surface() const noexcept {
    return impl_->cursor;
}
int Seat::cursor_hotspot_x() const noexcept {
    return impl_->cursor_hotspot_x;
}
int Seat::cursor_hotspot_y() const noexcept {
    return impl_->cursor_hotspot_y;
}

// --- touch ---

void Seat::touch_down(Surface& surface, int32_t id, double x, double y) {
    impl_->touch_focus = surface.id();
    wl_resource* res = surface.c_resource();
    wl_client* client = wl_resource_get_client(res);
    const uint32_t serial = impl_->next_serial();
    const uint32_t time = now_ms();
    for (wl_resource* t : impl_->touches) {
        if (wl_resource_get_client(t) == client) {
            wl_touch_send_down(t, serial, time, res, id, wl_fixed_from_double(x),
                               wl_fixed_from_double(y));
        }
    }
}

void Seat::touch_motion(int32_t id, double x, double y) {
    Surface* focus = surface_from_id(impl_->touch_focus);
    if (focus == nullptr) {
        return;
    }
    wl_client* client = client_of(*focus);
    const uint32_t time = now_ms();
    for (wl_resource* t : impl_->touches) {
        if (wl_resource_get_client(t) == client) {
            wl_touch_send_motion(t, time, id, wl_fixed_from_double(x), wl_fixed_from_double(y));
        }
    }
}

void Seat::touch_up(int32_t id) {
    Surface* focus = surface_from_id(impl_->touch_focus);
    if (focus == nullptr) {
        return;
    }
    wl_client* client = client_of(*focus);
    const uint32_t serial = impl_->next_serial();
    const uint32_t time = now_ms();
    for (wl_resource* t : impl_->touches) {
        if (wl_resource_get_client(t) == client) {
            wl_touch_send_up(t, serial, time, id);
        }
    }
}

void Seat::touch_frame() {
    for (wl_resource* t : impl_->touches) {
        if (wl_resource_get_version(t) >= WL_TOUCH_FRAME_SINCE_VERSION) {
            wl_touch_send_frame(t);
        }
    }
}

void Seat::touch_cancel() {
    for (wl_resource* t : impl_->touches) {
        if (wl_resource_get_version(t) >= WL_TOUCH_CANCEL_SINCE_VERSION) {
            wl_touch_send_cancel(t);
        }
    }
    impl_->touch_focus = {};
}

// --- drag and drop ---

void Seat::begin_drag(SeatDragHooks hooks) {
    // The pointer leaves the client for the duration of the drag: from here on
    // it only sees wl_data_device events.
    const SurfaceId focus = impl_->ptr_focus;
    pointer_clear_focus();
    impl_->drag = std::move(hooks);
    impl_->dragging = true;
    if (focus.valid() && impl_->drag.focus) {
        impl_->ptr_focus = focus;
        impl_->drag.focus(focus, 0, 0);
    }
}

void Seat::end_drag() {
    if (!impl_->dragging) {
        return;
    }
    impl_->dragging = false;
    impl_->drag = SeatDragHooks{};
    // Hand the pointer back to whatever it is over.
    const SurfaceId focus = impl_->ptr_focus;
    impl_->ptr_focus = {};
    if (Surface* surface = surface_from_id(focus); surface != nullptr) {
        pointer_enter(*surface, 0, 0);
    }
}

bool Seat::dragging() const noexcept {
    return impl_->dragging;
}

} // namespace luminaria
