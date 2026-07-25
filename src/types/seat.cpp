module;

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

module luminaria;

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

    Surface* kb_focus = nullptr;
    Surface* ptr_focus = nullptr;
    Surface* touch_focus = nullptr;
    // Focus pointers are raw, so we hold a destroy subscription for each and
    // clear the focus from it. This is what keeps them from dangling.
    Signal<SurfaceDestroy>::Connection kb_focus_gone;
    Signal<SurfaceDestroy>::Connection ptr_focus_gone;
    Signal<SurfaceDestroy>::Connection touch_focus_gone;

    Surface* cursor = nullptr;
    Signal<SurfaceDestroy>::Connection cursor_gone;
    int cursor_hotspot_x = 0;
    int cursor_hotspot_y = 0;

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

wl_client* client_of(Surface* surface) {
    return wl_resource_get_client(surface->c_resource());
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
    if (seat->ptr_focus == nullptr || client_of(seat->ptr_focus) != client) {
        return;
    }
    auto* surface = surface_resource != nullptr
                        ? static_cast<Surface*>(wl_resource_get_user_data(surface_resource))
                        : nullptr;
    seat->cursor = surface;
    seat->cursor_hotspot_x = hotspot_x;
    seat->cursor_hotspot_y = hotspot_y;
    seat->cursor_gone.disconnect();
    if (surface != nullptr) {
        seat->cursor_gone = surface->destroy.connect([seat](SurfaceDestroy&) {
            seat->cursor = nullptr;
            SeatCursorChange gone{nullptr, 0, 0};
            seat->cursor_changed.emit(gone);
        });
    }
    SeatCursorChange event{surface, hotspot_x, hotspot_y};
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
    if (impl_->kb_focus == surface) {
        return;
    }
    if (impl_->kb_focus != nullptr) {
        wl_resource* old = impl_->kb_focus->c_resource();
        wl_client* old_client = wl_resource_get_client(old);
        const uint32_t serial = impl_->next_serial();
        for (wl_resource* kb : impl_->keyboards) {
            if (wl_resource_get_client(kb) == old_client) {
                wl_keyboard_send_leave(kb, serial, old);
            }
        }
    }
    impl_->kb_focus = surface;
    impl_->kb_focus_gone.disconnect();
    if (surface != nullptr) {
        Impl* impl = impl_.get();
        impl_->kb_focus_gone = surface->destroy.connect([impl](SurfaceDestroy&) {
            // The focused surface is going away; drop the pointer without
            // sending leave (the resource is already being torn down).
            impl->kb_focus = nullptr;
            impl->kb_focus_gone.disconnect();
            SeatKeyboardFocus event{nullptr};
            impl->keyboard_focus_changed.emit(event);
        });
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
    SeatKeyboardFocus event{surface};
    impl_->keyboard_focus_changed.emit(event);
}

Surface* Seat::keyboard_focus() const noexcept {
    return impl_->kb_focus;
}

Signal<SeatKeyboardFocus>& Seat::keyboard_focus_changed() noexcept {
    return impl_->keyboard_focus_changed;
}

void Seat::notify_key(uint32_t key, bool pressed) {
    if (impl_->kb_focus == nullptr) {
        return;
    }
    wl_client* client = client_of(impl_->kb_focus);
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
    if (impl_->kb_focus == nullptr) {
        return;
    }
    wl_client* client = client_of(impl_->kb_focus);
    const uint32_t serial = impl_->next_serial();
    for (wl_resource* kb : impl_->keyboards) {
        if (wl_resource_get_client(kb) == client) {
            wl_keyboard_send_modifiers(kb, serial, depressed, latched, locked, group);
        }
    }
}

// --- pointer ---

void Seat::pointer_enter(Surface& surface, double sx, double sy) {
    if (impl_->dragging) {
        // During a drag the pointer belongs to the drag: no wl_pointer events.
        if (impl_->ptr_focus != &surface) {
            impl_->ptr_focus = &surface;
            Impl* impl = impl_.get();
            impl_->ptr_focus_gone = surface.destroy.connect([impl](SurfaceDestroy&) {
                impl->ptr_focus = nullptr;
                impl->ptr_focus_gone.disconnect();
            });
            if (impl_->drag.focus) {
                impl_->drag.focus(&surface, sx, sy);
            }
        } else if (impl_->drag.motion) {
            impl_->drag.motion(sx, sy);
        }
        return;
    }
    if (impl_->ptr_focus == &surface) {
        pointer_motion(sx, sy);
        return;
    }
    pointer_clear_focus();

    impl_->ptr_focus = &surface;
    Impl* impl = impl_.get();
    impl_->ptr_focus_gone = surface.destroy.connect([impl](SurfaceDestroy&) {
        impl->ptr_focus = nullptr;
        impl->ptr_focus_gone.disconnect();
        // A cursor set by that client is meaningless now.
        if (impl->cursor != nullptr) {
            impl->cursor = nullptr;
            impl->cursor_gone.disconnect();
            SeatCursorChange gone{nullptr, 0, 0};
            impl->cursor_changed.emit(gone);
        }
        SeatPointerFocus event{nullptr};
        impl->pointer_focus_changed.emit(event);
    });

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
    SeatPointerFocus event{&surface};
    impl_->pointer_focus_changed.emit(event);
}

void Seat::pointer_clear_focus() {
    if (impl_->ptr_focus == nullptr) {
        return;
    }
    if (impl_->dragging) {
        impl_->ptr_focus = nullptr;
        impl_->ptr_focus_gone.disconnect();
        if (impl_->drag.focus) {
            impl_->drag.focus(nullptr, 0, 0);
        }
        return;
    }
    wl_resource* res = impl_->ptr_focus->c_resource();
    wl_client* client = wl_resource_get_client(res);
    const uint32_t serial = impl_->next_serial();
    for (wl_resource* p : impl_->pointers) {
        if (wl_resource_get_client(p) == client) {
            wl_pointer_send_leave(p, serial, res);
            pointer_frame_if_supported(p);
        }
    }
    impl_->ptr_focus = nullptr;
    impl_->ptr_focus_gone.disconnect();
    // The cursor image belonged to that client.
    if (impl_->cursor != nullptr) {
        impl_->cursor = nullptr;
        impl_->cursor_gone.disconnect();
        SeatCursorChange gone{nullptr, 0, 0};
        impl_->cursor_changed.emit(gone);
    }
    SeatPointerFocus event{nullptr};
    impl_->pointer_focus_changed.emit(event);
}

Surface* Seat::pointer_focus() const noexcept {
    return impl_->ptr_focus;
}

Signal<SeatPointerFocus>& Seat::pointer_focus_changed() noexcept {
    return impl_->pointer_focus_changed;
}

void Seat::pointer_motion(double sx, double sy) {
    if (impl_->ptr_focus == nullptr) {
        return;
    }
    if (impl_->dragging) {
        if (impl_->drag.motion) {
            impl_->drag.motion(sx, sy);
        }
        return;
    }
    wl_client* client = client_of(impl_->ptr_focus);
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
    if (impl_->ptr_focus == nullptr) {
        return;
    }
    wl_client* client = client_of(impl_->ptr_focus);
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
    if (impl_->ptr_focus == nullptr || impl_->dragging || (dx == 0.0 && dy == 0.0)) {
        return;
    }
    wl_client* client = client_of(impl_->ptr_focus);
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
    if (impl_->ptr_focus == nullptr || impl_->dragging ||
        (dx_steps == 0 && dy_steps == 0)) {
        return;
    }
    // One wheel notch is 10 surface units by convention (what GTK/Qt expect).
    constexpr double kStep = 10.0;
    wl_client* client = client_of(impl_->ptr_focus);
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
    if (impl_->ptr_focus == nullptr || impl_->dragging || (!horizontal && !vertical)) {
        return;
    }
    wl_client* client = client_of(impl_->ptr_focus);
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
Surface* Seat::cursor_surface() const noexcept {
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
    if (impl_->touch_focus != &surface) {
        impl_->touch_focus = &surface;
        Impl* impl = impl_.get();
        impl_->touch_focus_gone = surface.destroy.connect([impl](SurfaceDestroy&) {
            impl->touch_focus = nullptr;
            impl->touch_focus_gone.disconnect();
        });
    }
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
    if (impl_->touch_focus == nullptr) {
        return;
    }
    wl_client* client = client_of(impl_->touch_focus);
    const uint32_t time = now_ms();
    for (wl_resource* t : impl_->touches) {
        if (wl_resource_get_client(t) == client) {
            wl_touch_send_motion(t, time, id, wl_fixed_from_double(x), wl_fixed_from_double(y));
        }
    }
}

void Seat::touch_up(int32_t id) {
    if (impl_->touch_focus == nullptr) {
        return;
    }
    wl_client* client = client_of(impl_->touch_focus);
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
    impl_->touch_focus = nullptr;
    impl_->touch_focus_gone.disconnect();
}

// --- drag and drop ---

void Seat::begin_drag(SeatDragHooks hooks) {
    // The pointer leaves the client for the duration of the drag: from here on
    // it only sees wl_data_device events.
    Surface* focus = impl_->ptr_focus;
    pointer_clear_focus();
    impl_->drag = std::move(hooks);
    impl_->dragging = true;
    if (focus != nullptr && impl_->drag.focus) {
        impl_->ptr_focus = focus;
        Impl* impl = impl_.get();
        impl_->ptr_focus_gone = focus->destroy.connect([impl](SurfaceDestroy&) {
            impl->ptr_focus = nullptr;
            impl->ptr_focus_gone.disconnect();
        });
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
    Surface* focus = impl_->ptr_focus;
    impl_->ptr_focus = nullptr;
    impl_->ptr_focus_gone.disconnect();
    if (focus != nullptr) {
        pointer_enter(*focus, 0, 0);
    }
}

bool Seat::dragging() const noexcept {
    return impl_->dragging;
}

} // namespace luminaria
