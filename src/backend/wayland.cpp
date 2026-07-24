#include "luminaria/backend/wayland.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>

#include "xdg-shell-client-protocol.h"

namespace luminaria {

namespace {

uint8_t to_u8(float c) {
    const float v = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

// Per-buffer state, freed when the parent releases the wl_buffer.
struct ShmBuffer {
    wl_buffer* buffer;
    void* data;
    size_t size;
};
void buffer_release(void* data, wl_buffer* buffer) {
    auto* sb = static_cast<ShmBuffer*>(data);
    wl_buffer_destroy(buffer);
    munmap(sb->data, sb->size);
    delete sb;
}
const wl_buffer_listener kBufferListener{buffer_release};

// Allocate an XRGB8888 shm buffer; on success sets *out_pixels to the w*h u32
// mapping for the caller to fill. The buffer frees itself on parent release.
wl_buffer* make_shm_buffer(wl_shm* shm, int w, int h, uint32_t** out_pixels) {
    const int stride = w * 4;
    const size_t size = static_cast<size_t>(stride) * h;
    int fd = memfd_create("luminaria-nested", MFD_CLOEXEC);
    if (fd < 0) {
        return nullptr;
    }
    wl_buffer* buffer = nullptr;
    if (ftruncate(fd, static_cast<off_t>(size)) == 0) {
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) {
            wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(size));
            buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
            wl_shm_pool_destroy(pool);
            auto* sb = new ShmBuffer{buffer, p, size};
            wl_buffer_add_listener(buffer, &kBufferListener, sb);
            *out_pixels = static_cast<uint32_t*>(p);
        }
    }
    close(fd);
    return buffer;
}

wl_buffer* make_solid_buffer(wl_shm* shm, int w, int h, Color color) {
    uint32_t* px = nullptr;
    wl_buffer* buffer = make_shm_buffer(shm, w, h, &px);
    if (buffer == nullptr) {
        return nullptr;
    }
    const uint32_t argb = (static_cast<uint32_t>(to_u8(color.r)) << 16) |
                          (static_cast<uint32_t>(to_u8(color.g)) << 8) |
                          static_cast<uint32_t>(to_u8(color.b));
    std::fill_n(px, static_cast<size_t>(w) * h, argb);
    return buffer;
}

class WaylandOutput final : public Output {
public:
    wl_display* parent;
    wl_shm* shm;
    wl_surface* surface;
    xdg_surface* xsurf = nullptr;
    xdg_toplevel* toplevel = nullptr;
    bool configured = false;

    WaylandOutput(wl_display* parent, wl_shm* shm, wl_surface* surface, int w, int h)
        : Output(w, h), parent(parent), shm(shm), surface(surface) {}

    Status commit(Color color) override {
        wl_buffer* buffer = make_solid_buffer(shm, width_, height_, color);
        if (buffer == nullptr) {
            return fail("nested: buffer allocation failed");
        }
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, width_, height_);
        request_frame();
        wl_surface_commit(surface);
        wl_display_flush(parent);
        return ok();
    }

    // Present a composited RGBA frame: pack into an XRGB8888 shm buffer and
    // hand it to the parent. This is what makes client windows visible nested.
    Status commit_frame(std::span<const Pixel> rgba, int w, int h) override {
        if (w != width_ || h != height_ ||
            rgba.size() < static_cast<size_t>(w) * static_cast<size_t>(h)) {
            return fail("nested: frame size mismatch");
        }
        uint32_t* px = nullptr;
        wl_buffer* buffer = make_shm_buffer(shm, w, h, &px);
        if (buffer == nullptr) {
            return fail("nested: buffer allocation failed");
        }
        const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
        for (size_t i = 0; i < n; ++i) {
            px[i] = (static_cast<uint32_t>(rgba[i].r) << 16) |
                    (static_cast<uint32_t>(rgba[i].g) << 8) |
                    static_cast<uint32_t>(rgba[i].b);
        }
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, w, h);
        request_frame();
        wl_surface_commit(surface);
        wl_display_flush(parent);
        return ok();
    }

    void request_frame() {
        wl_callback* cb = wl_surface_frame(surface);
        wl_callback_add_listener(cb, &frame_listener_, this);
    }

private:
    static void on_frame(void* data, wl_callback* cb, uint32_t) {
        auto* self = static_cast<WaylandOutput*>(data);
        wl_callback_destroy(cb);
        FrameEvent event{*self};
        self->frame.emit(event);
    }
    static constexpr wl_callback_listener frame_listener_{on_frame};
};

} // namespace

struct WaylandBackend::Impl {
    EventLoop loop;
    wl_display* parent = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    WaylandBackend* owner = nullptr; // set in start(), after all moves; emits input signals
    EventSource fd_source;
    std::vector<std::unique_ptr<WaylandOutput>> outputs;

    ~Impl() {
        outputs.clear();
        if (keyboard != nullptr) {
            wl_keyboard_destroy(keyboard);
        }
        if (pointer != nullptr) {
            wl_pointer_destroy(pointer);
        }
        if (seat != nullptr) {
            wl_seat_destroy(seat);
        }
        if (wm_base != nullptr) {
            xdg_wm_base_destroy(wm_base);
        }
        if (shm != nullptr) {
            wl_shm_destroy(shm);
        }
        if (compositor != nullptr) {
            wl_compositor_destroy(compositor);
        }
        if (registry != nullptr) {
            wl_registry_destroy(registry);
        }
        if (parent != nullptr) {
            wl_display_disconnect(parent);
        }
    }
};

namespace {

void wm_base_ping(void*, xdg_wm_base* wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}
const xdg_wm_base_listener kWmBaseListener{wm_base_ping};

// ---- parent input: forward pointer/keyboard into the backend's signals ----
// data is Impl*; owner is set by the time these fire (start() ran first).
void emit_motion(WaylandBackend::Impl* impl, double x, double y) {
    if (impl->owner != nullptr) {
        PointerMotionAbsEvent e{x, y};
        impl->owner->pointer_motion.emit(e);
    }
}
void ptr_enter(void* data, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t sx, wl_fixed_t sy) {
    emit_motion(static_cast<WaylandBackend::Impl*>(data), wl_fixed_to_double(sx),
                wl_fixed_to_double(sy));
}
void ptr_leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
    emit_motion(static_cast<WaylandBackend::Impl*>(data), -1.0, -1.0); // clear focus
}
void ptr_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    emit_motion(static_cast<WaylandBackend::Impl*>(data), wl_fixed_to_double(sx),
                wl_fixed_to_double(sy));
}
void ptr_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if (impl->owner != nullptr) {
        PointerButtonEvent e{button, state == WL_POINTER_BUTTON_STATE_PRESSED};
        impl->owner->pointer_button.emit(e);
    }
}
void ptr_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
const wl_pointer_listener kPointerListener{ptr_enter, ptr_leave, ptr_motion, ptr_button, ptr_axis};

void kb_keymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); }
void kb_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
void kb_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
void kb_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if (impl->owner != nullptr) {
        KeyEvent e{key, state == WL_KEYBOARD_KEY_STATE_PRESSED};
        impl->owner->key.emit(e);
    }
}
void kb_mods(void* data, wl_keyboard*, uint32_t, uint32_t dep, uint32_t lat, uint32_t lock,
             uint32_t grp) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if (impl->owner != nullptr) {
        ModifiersEvent e{dep, lat, lock, grp};
        impl->owner->modifiers.emit(e);
    }
}
void kb_repeat(void*, wl_keyboard*, int32_t, int32_t) {}
const wl_keyboard_listener kKeyboardListener{kb_keymap, kb_enter, kb_leave,
                                             kb_key,    kb_mods,  kb_repeat};

void seat_caps(void* data, wl_seat* seat, uint32_t caps) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) != 0 && impl->pointer == nullptr) {
        impl->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(impl->pointer, &kPointerListener, impl);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && impl->keyboard == nullptr) {
        impl->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(impl->keyboard, &kKeyboardListener, impl);
    }
}
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener kSeatListener{seat_caps, seat_name};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        impl->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "xdg_wm_base") == 0) {
        impl->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(impl->wm_base, &kWmBaseListener, impl);
    } else if (std::strcmp(interface, "wl_shm") == 0) {
        impl->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, "wl_seat") == 0) {
        // Bind v1: we only consume enter/leave/motion/button/axis. Higher versions
        // send frame/axis_source/etc — libwayland aborts if a listener slot for a
        // sent event is null, so don't ask for events we don't handle.
        impl->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        wl_seat_add_listener(impl->seat, &kSeatListener, impl);
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

void xdg_surface_configure(void* data, xdg_surface* xsurf, uint32_t serial) {
    auto* out = static_cast<WaylandOutput*>(data);
    xdg_surface_ack_configure(xsurf, serial);
    out->configured = true;
    wl_display_flush(out->parent);
}
const xdg_surface_listener kXdgSurfaceListener{xdg_surface_configure};

void toplevel_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
void toplevel_close(void*, xdg_toplevel*) {}
const xdg_toplevel_listener kToplevelListener{toplevel_configure, toplevel_close};

} // namespace

WaylandBackend::WaylandBackend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
WaylandBackend::~WaylandBackend() = default;
WaylandBackend::WaylandBackend(WaylandBackend&&) noexcept = default;
WaylandBackend& WaylandBackend::operator=(WaylandBackend&&) noexcept = default;

Result<WaylandBackend> WaylandBackend::create(EventLoop loop) {
    wl_display* parent = wl_display_connect(nullptr);
    if (parent == nullptr) {
        return fail("nested: no parent compositor (WAYLAND_DISPLAY)");
    }
    auto impl = std::make_unique<Impl>();
    impl->loop = loop;
    impl->parent = parent;
    impl->registry = wl_display_get_registry(parent);
    wl_registry_add_listener(impl->registry, &kRegistryListener, impl.get());
    wl_display_roundtrip(parent); // receive globals

    if (impl->compositor == nullptr || impl->wm_base == nullptr || impl->shm == nullptr) {
        return fail("nested: parent lacks wl_compositor/xdg_wm_base/wl_shm");
    }
    return WaylandBackend{std::move(impl)};
}

Output& WaylandBackend::add_output(int width, int height) {
    auto out = std::make_unique<WaylandOutput>(impl_->parent, impl_->shm,
                                               wl_compositor_create_surface(impl_->compositor),
                                               width, height);
    out->xsurf = xdg_wm_base_get_xdg_surface(impl_->wm_base, out->surface);
    xdg_surface_add_listener(out->xsurf, &kXdgSurfaceListener, out.get());
    out->toplevel = xdg_surface_get_toplevel(out->xsurf);
    xdg_toplevel_add_listener(out->toplevel, &kToplevelListener, out.get());
    xdg_toplevel_set_title(out->toplevel, "luminaria");
    wl_surface_commit(out->surface); // initial commit -> parent sends configure
    wl_display_flush(impl_->parent);

    Output& ref = *out;
    impl_->outputs.push_back(std::move(out));
    return ref;
}

Status WaylandBackend::start() {
    impl_->owner = this; // this object is now at its final address; input can emit
    // Drive the parent connection from our event loop.
    impl_->fd_source = impl_->loop.add_fd(wl_display_get_fd(impl_->parent), [this] {
        if (wl_display_dispatch(impl_->parent) < 0) {
            impl_->fd_source.disarm(); // parent gone; stop polling
        }
    });

    // Block until every output has had its first configure.
    for (int tries = 0; tries < 100; ++tries) {
        const bool all = std::ranges::all_of(
            impl_->outputs, [](const std::unique_ptr<WaylandOutput>& o) { return o->configured; });
        if (all) {
            break;
        }
        wl_display_roundtrip(impl_->parent);
    }

    for (auto& out : impl_->outputs) {
        NewOutput event{*out};
        new_output.emit(event);
    }
    // Kick the first frame so the compositor commits initial content (which maps
    // the window); subsequent frames come from parent frame callbacks.
    for (auto& out : impl_->outputs) {
        WaylandOutput* raw = out.get();
        impl_->loop.once([raw] {
            FrameEvent event{*raw};
            raw->frame.emit(event);
        });
    }
    wl_display_flush(impl_->parent);
    return ok();
}

} // namespace luminaria
