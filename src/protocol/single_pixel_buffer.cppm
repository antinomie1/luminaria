// luminaria/single_pixel_buffer.cppm — the wp_single_pixel_buffer_manager_v1 global.
//
// A 1×1 solid-colour wl_buffer with no shm pool and no GPU allocation behind it.
// Clients use it for backdrops, dimming layers and letterbox fills, normally
// stretched to size through wp_viewporter — without this global they fall back
// to allocating a full-screen buffer just to paint one colour.
//
// Importing luminaria pulls in no libwayland headers: the C types this
// interface names are forward-declared in the global module fragment.

module;

#include "detail/wayland_fwd.h"
#include <cstdint>
#include <memory>

#include <utility>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "single-pixel-buffer-v1-protocol.h"

export module luminaria:single_pixel_buffer;

import :display;
import :expected;

export namespace luminaria {

class Display;

/// The wp_single_pixel_buffer_manager_v1 global (version 1). Move-only;
/// pointer-stable state so the libwayland global can hold a pointer to it.
class SinglePixelBufferManager {
public:
    [[nodiscard]] static Result<SinglePixelBufferManager> create(Display& display);

    ~SinglePixelBufferManager();
    SinglePixelBufferManager(SinglePixelBufferManager&&) noexcept;
    SinglePixelBufferManager& operator=(SinglePixelBufferManager&&) noexcept;
    SinglePixelBufferManager(const SinglePixelBufferManager&) = delete;
    SinglePixelBufferManager& operator=(const SinglePixelBufferManager&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit SinglePixelBufferManager(std::unique_ptr<Impl> impl) noexcept;
};

/// If `buffer` is a single-pixel wl_buffer, write its colour into `rgba` as
/// RGBA8 and return true; false for any other buffer. Alpha is pre-multiplied,
/// same convention as the wl_shm ARGB8888 path.
[[nodiscard]] bool single_pixel_buffer_color(wl_resource* buffer, std::uint8_t rgba[4]);

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Implements wp_single_pixel_buffer_manager_v1 (version 1). The wl_buffer it
// mints owns nothing but four bytes: there is no pool, no fd and no GPU
// allocation, so the "import" is just reading them back out again.

namespace luminaria {

namespace {

struct SinglePixel {
    std::uint8_t rgba[4];
};

void buffer_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void buffer_resource_destroy(wl_resource* resource) {
    delete static_cast<SinglePixel*>(wl_resource_get_user_data(resource));
}

constexpr struct wl_buffer_interface single_pixel_wl_buffer_impl = {
    .destroy = buffer_destroy_request,
};

void manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void manager_create_u32_rgba_buffer(wl_client* client, wl_resource* manager, uint32_t id,
                                    uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    wl_resource* resource = wl_resource_create(
        client, &wl_buffer_interface, 1, static_cast<int>(id));
    if (resource == nullptr) {
        wl_resource_post_no_memory(manager);
        return;
    }
    // The protocol carries each channel as a 32-bit fraction of UINT32_MAX;
    // our pixel pipeline is 8-bit, so keep the top byte.
    auto* px = new SinglePixel{{static_cast<std::uint8_t>(r >> 24),
                                static_cast<std::uint8_t>(g >> 24),
                                static_cast<std::uint8_t>(b >> 24),
                                static_cast<std::uint8_t>(a >> 24)}};
    wl_resource_set_implementation(resource, &single_pixel_wl_buffer_impl, px,
                                   buffer_resource_destroy);
}

constexpr struct wp_single_pixel_buffer_manager_v1_interface manager_impl = {
    .destroy = manager_destroy_request,
    .create_u32_rgba_buffer = manager_create_u32_rgba_buffer,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(
        client, &wp_single_pixel_buffer_manager_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

} // namespace

struct SinglePixelBufferManager::Impl {
    wl_global* global = nullptr;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

SinglePixelBufferManager::SinglePixelBufferManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
SinglePixelBufferManager::~SinglePixelBufferManager() = default;
SinglePixelBufferManager::SinglePixelBufferManager(SinglePixelBufferManager&&) noexcept = default;
SinglePixelBufferManager& SinglePixelBufferManager::operator=(SinglePixelBufferManager&&) noexcept =
    default;

Result<SinglePixelBufferManager> SinglePixelBufferManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->global = wl_global_create(display.c_ptr(),
                                    &wp_single_pixel_buffer_manager_v1_interface, 1, impl.get(),
                                    manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wp_single_pixel_buffer_manager_v1) failed");
    }
    return SinglePixelBufferManager{std::move(impl)};
}

bool single_pixel_buffer_color(wl_resource* buffer, std::uint8_t rgba[4]) {
    if (buffer == nullptr ||
        !wl_resource_instance_of(buffer, &wl_buffer_interface, &single_pixel_wl_buffer_impl)) {
        return false;
    }
    const auto* px = static_cast<const SinglePixel*>(wl_resource_get_user_data(buffer));
    for (int i = 0; i < 4; ++i) {
        rgba[i] = px->rgba[i];
    }
    return true;
}

} // namespace luminaria
