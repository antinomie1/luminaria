#include "luminaria/seat.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"

namespace luminaria {

struct Seat::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    std::string name;
    std::string keymap; // xkb keymap text (null-terminated payload sent to clients)

    std::vector<wl_resource*> keyboards;
    std::vector<wl_resource*> pointers;
    Surface* kb_focus = nullptr;
    Surface* ptr_focus = nullptr;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }

    [[nodiscard]] uint32_t next_serial() const { return wl_display_next_serial(display); }
};

namespace {

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

// ---- wl_keyboard / wl_pointer resource lifecycle ----
void resource_release(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
constexpr struct wl_keyboard_interface keyboard_impl = {.release = resource_release};

void keyboard_destroy(wl_resource* resource) {
    auto* seat = static_cast<Seat::Impl*>(wl_resource_get_user_data(resource));
    std::erase(seat->keyboards, resource);
}

void pointer_set_cursor(wl_client*, wl_resource*, uint32_t, wl_resource*, int32_t, int32_t) {}
constexpr struct wl_pointer_interface pointer_impl = {.set_cursor = pointer_set_cursor,
                                                      .release = resource_release};
void pointer_destroy(wl_resource* resource) {
    auto* seat = static_cast<Seat::Impl*>(wl_resource_get_user_data(resource));
    std::erase(seat->pointers, resource);
}

// ---- wl_seat requests ----
void seat_get_pointer(wl_client* client, wl_resource* seat_resource, uint32_t id) {
    auto* seat = static_cast<Seat::Impl*>(wl_resource_get_user_data(seat_resource));
    wl_resource* resource = wl_resource_create(client, &wl_pointer_interface,
                                               wl_resource_get_version(seat_resource), id);
    wl_resource_set_implementation(resource, &pointer_impl, seat, pointer_destroy);
    seat->pointers.push_back(resource);
}
void seat_get_keyboard(wl_client* client, wl_resource* seat_resource, uint32_t id) {
    auto* seat = static_cast<Seat::Impl*>(wl_resource_get_user_data(seat_resource));
    wl_resource* resource = wl_resource_create(client, &wl_keyboard_interface,
                                               wl_resource_get_version(seat_resource), id);
    wl_resource_set_implementation(resource, &keyboard_impl, seat, keyboard_destroy);
    seat->keyboards.push_back(resource);
    send_keymap(seat, resource);
    if (wl_resource_get_version(resource) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) {
        wl_keyboard_send_repeat_info(resource, 25, 600);
    }
}
void seat_get_touch(wl_client* client, wl_resource* seat_resource, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_touch_interface,
                                               wl_resource_get_version(seat_resource), id);
    wl_resource_set_implementation(resource, nullptr, nullptr, nullptr);
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

void seat_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* seat = static_cast<Seat::Impl*>(data);
    wl_resource* resource =
        wl_resource_create(client, &wl_seat_interface, static_cast<int>(version), id);
    wl_resource_set_implementation(resource, &seat_impl, seat, nullptr);
    wl_seat_send_capabilities(resource,
                              WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);
    if (version >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(resource, seat->name.c_str());
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
            wl_keyboard_send_key(kb, serial, 0, key, state);
        }
    }
}

namespace {
void pointer_frame_if_supported(wl_resource* pointer) {
    if (wl_resource_get_version(pointer) >= WL_POINTER_FRAME_SINCE_VERSION) {
        wl_pointer_send_frame(pointer);
    }
}
} // namespace

void Seat::pointer_enter(Surface& surface, double sx, double sy) {
    impl_->ptr_focus = &surface;
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
}

void Seat::pointer_motion(double sx, double sy) {
    if (impl_->ptr_focus == nullptr) {
        return;
    }
    wl_client* client = client_of(impl_->ptr_focus);
    for (wl_resource* p : impl_->pointers) {
        if (wl_resource_get_client(p) == client) {
            wl_pointer_send_motion(p, 0, wl_fixed_from_double(sx), wl_fixed_from_double(sy));
            pointer_frame_if_supported(p);
        }
    }
}

void Seat::pointer_button(uint32_t button, bool pressed) {
    if (impl_->ptr_focus == nullptr) {
        return;
    }
    wl_client* client = client_of(impl_->ptr_focus);
    const uint32_t serial = impl_->next_serial();
    const uint32_t state =
        pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
    for (wl_resource* p : impl_->pointers) {
        if (wl_resource_get_client(p) == client) {
            wl_pointer_send_button(p, serial, 0, button, state);
            pointer_frame_if_supported(p);
        }
    }
}

} // namespace luminaria
