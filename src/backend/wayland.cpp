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

wl_buffer* make_solid_buffer(wl_shm* shm, int w, int h, Color color) {
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
            const uint32_t argb = (static_cast<uint32_t>(to_u8(color.r)) << 16) |
                                  (static_cast<uint32_t>(to_u8(color.g)) << 8) |
                                  static_cast<uint32_t>(to_u8(color.b));
            std::fill_n(static_cast<uint32_t*>(p), static_cast<size_t>(w) * h, argb);
            wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(size));
            buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
            wl_shm_pool_destroy(pool);
            auto* sb = new ShmBuffer{buffer, p, size};
            wl_buffer_add_listener(buffer, &kBufferListener, sb);
        }
    }
    close(fd);
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
    EventSource fd_source;
    std::vector<std::unique_ptr<WaylandOutput>> outputs;

    ~Impl() {
        outputs.clear();
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
