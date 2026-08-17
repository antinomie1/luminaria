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
#include <typeinfo>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "single-pixel-buffer-v1-protocol.h"

export module luminaria:single_pixel_buffer;

import std;

import :client_buffer;
import :display;
import :expected;
import :protocol_helper;

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

struct SinglePixel final : ClientBuffer {
    std::uint8_t pixel[4];

    [[nodiscard]] int width() const noexcept override { return 1; }
    [[nodiscard]] int height() const noexcept override { return 1; }
    [[nodiscard]] bool rgba(std::vector<std::uint8_t>& out, int& w, int& h) const override {
        out.assign(pixel, pixel + 4);
        w = 1;
        h = 1;
        return true;
    }
};

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
    auto px = std::make_unique<SinglePixel>();
    px->pixel[0] = static_cast<std::uint8_t>(r >> 24);
    px->pixel[1] = static_cast<std::uint8_t>(g >> 24);
    px->pixel[2] = static_cast<std::uint8_t>(b >> 24);
    px->pixel[3] = static_cast<std::uint8_t>(a >> 24);
    install_client_buffer(resource, std::move(px));
}

constexpr struct wp_single_pixel_buffer_manager_v1_interface manager_impl = {
    .destroy = resource_destroy_request,
    .create_u32_rgba_buffer = manager_create_u32_rgba_buffer,
};

} // namespace

struct SinglePixelBufferManager::Impl {
    WlGlobal global;
};

SinglePixelBufferManager::SinglePixelBufferManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
SinglePixelBufferManager::~SinglePixelBufferManager() = default;
SinglePixelBufferManager::SinglePixelBufferManager(SinglePixelBufferManager&&) noexcept = default;
SinglePixelBufferManager& SinglePixelBufferManager::operator=(SinglePixelBufferManager&&) noexcept =
    default;

Result<SinglePixelBufferManager> SinglePixelBufferManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    auto global = create_wl_global<&wp_single_pixel_buffer_manager_v1_interface,
                                   default_bind<&wp_single_pixel_buffer_manager_v1_interface,
                                                &manager_impl>>(display, 1, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return SinglePixelBufferManager{std::move(impl)};
}

bool single_pixel_buffer_color(wl_resource* buffer, std::uint8_t rgba[4]) {
    auto* px = dynamic_cast<SinglePixel*>(client_buffer_from_resource(buffer));
    if (px == nullptr) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        rgba[i] = px->pixel[i];
    }
    return true;
}

} // namespace luminaria
