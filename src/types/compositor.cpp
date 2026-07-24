#include "luminaria/compositor.hpp"

#include <ctime>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "luminaria/core/display.hpp"

namespace luminaria {

struct Compositor::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    Signal<NewSurface> new_surface;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

bool Surface::current_buffer_rgba(std::vector<std::uint8_t>& out, int& width, int& height) const {
    if (current_buffer_ == nullptr) {
        return false;
    }
    wl_shm_buffer* shm = wl_shm_buffer_get(current_buffer_);
    if (shm == nullptr) {
        return false; // not an shm buffer (e.g. dmabuf) — unsupported here
    }
    const uint32_t format = wl_shm_buffer_get_format(shm);
    if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888) {
        return false;
    }
    const int w = wl_shm_buffer_get_width(shm);
    const int h = wl_shm_buffer_get_height(shm);
    const int stride = wl_shm_buffer_get_stride(shm);
    const bool opaque = format == WL_SHM_FORMAT_XRGB8888;

    wl_shm_buffer_begin_access(shm);
    const auto* data = static_cast<const uint8_t*>(wl_shm_buffer_get_data(shm));
    out.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = data + static_cast<size_t>(y) * stride;
        uint8_t* dst = out.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            // shm ARGB8888 is little-endian: bytes are B,G,R,A. Emit RGBA.
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = opaque ? 255 : src[x * 4 + 3];
        }
    }
    wl_shm_buffer_end_access(shm);
    width = w;
    height = h;
    return true;
}

namespace {
uint32_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
} // namespace

void Surface::apply_commit() {
    if (pending_buffer_dirty_) {
        // Release the buffer we're replacing so the client can reuse it. We copy
        // buffer contents at render time, so we only need to hold the current one.
        if (current_buffer_ != nullptr && current_buffer_ != pending_buffer_) {
            wl_buffer_send_release(current_buffer_);
        }
        current_buffer_ = pending_buffer_;
        pending_buffer_dirty_ = false;
    }
    SurfaceCommit event{*this};
    commit.emit(event);

    // Ack frame callbacks so the client draws its next frame.
    // TODO: fired on commit rather than on presentation — fine for now; pace
    // via the output frame if a client spins.
    const uint32_t time = now_ms();
    for (wl_resource* cb : frame_callbacks_) {
        wl_callback_send_done(cb, time);
        wl_resource_destroy(cb);
    }
    frame_callbacks_.clear();
}

namespace {

Surface* surface_of(wl_resource* resource) {
    return static_cast<Surface*>(wl_resource_get_user_data(resource));
}

void surface_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void surface_attach(wl_client*, wl_resource* resource, wl_resource* buffer, int32_t, int32_t) {
    surface_of(resource)->set_pending_buffer(buffer);
}
void surface_commit(wl_client*, wl_resource* resource) {
    surface_of(resource)->apply_commit();
}
void surface_frame(wl_client* client, wl_resource* resource, uint32_t callback_id) {
    wl_resource* cb = wl_resource_create(client, &wl_callback_interface, 1, callback_id);
    if (cb != nullptr) {
        surface_of(resource)->add_frame_callback(cb);
    }
}
// Requests we accept but don't act on yet (damage tracking / regions / transforms).
void surface_noop_damage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void surface_noop_region(wl_client*, wl_resource*, wl_resource*) {}
void surface_noop_i32(wl_client*, wl_resource*, int32_t) {}
void surface_noop_offset(wl_client*, wl_resource*, int32_t, int32_t) {}

// TODO: all requests wired so real clients don't hit a null slot; damage /
// regions / transform / scale / offset are accepted no-ops for now.
constexpr struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy_request,
    .attach = surface_attach,
    .damage = surface_noop_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_noop_region,
    .set_input_region = surface_noop_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_noop_i32,
    .set_buffer_scale = surface_noop_i32,
    .damage_buffer = surface_noop_damage,
    .offset = surface_noop_offset,
};

void surface_resource_destroy(wl_resource* resource) {
    delete surface_of(resource);
}

void compositor_create_surface(wl_client* client, wl_resource* compositor_resource, uint32_t id) {
    auto* impl = static_cast<Compositor::Impl*>(wl_resource_get_user_data(compositor_resource));
    wl_resource* resource = wl_resource_create(
        client, &wl_surface_interface, wl_resource_get_version(compositor_resource), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* surface = new Surface{resource};
    wl_resource_set_implementation(resource, &surface_impl, surface, surface_resource_destroy);

    NewSurface event{*surface};
    impl->new_surface.emit(event);
}

// Minimal wl_region: all requests wired (add/subtract accepted no-ops) so clients
// that build opaque/input regions don't hit a null slot.
// TODO: track the region geometry when input/opaque regions actually matter.
void region_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void region_add(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void region_subtract(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
constexpr struct wl_region_interface region_impl = {
    .destroy = region_destroy_request,
    .add = region_add,
    .subtract = region_subtract,
};

void compositor_create_region(wl_client* client, wl_resource* compositor_resource, uint32_t id) {
    wl_resource* resource = wl_resource_create(
        client, &wl_region_interface, wl_resource_get_version(compositor_resource), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &region_impl, nullptr, nullptr);
}

constexpr struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

void compositor_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_compositor_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &compositor_impl, data, nullptr);
}

} // namespace

Compositor::Compositor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Compositor::~Compositor() = default;
Compositor::Compositor(Compositor&&) noexcept = default;
Compositor& Compositor::operator=(Compositor&&) noexcept = default;

Result<Compositor> Compositor::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    // Version 4: covers attach/damage/commit/scale/transform; enough for now.
    impl->global = wl_global_create(impl->display, &wl_compositor_interface, 4, impl.get(),
                                    compositor_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wl_compositor) failed");
    }
    return Compositor{std::move(impl)};
}

Signal<NewSurface>& Compositor::new_surface() noexcept {
    return impl_->new_surface;
}

} // namespace luminaria
