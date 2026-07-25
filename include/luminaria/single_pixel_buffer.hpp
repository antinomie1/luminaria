// luminaria/single_pixel_buffer.hpp — the wp_single_pixel_buffer_manager_v1 global.
//
// A 1×1 solid-colour wl_buffer with no shm pool and no GPU allocation behind it.
// Clients use it for backdrops, dimming layers and letterbox fills, normally
// stretched to size through wp_viewporter — without this global they fall back
// to allocating a full-screen buffer just to paint one colour.
//
// Public header stays C-header-free.
#pragma once

#include <cstdint>
#include <memory>

#include "luminaria/core/expected.hpp"

struct wl_resource;

namespace luminaria {

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
