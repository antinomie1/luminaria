// luminaria/util/pixel_layout.cppm — is a client-declared buffer layout safe to touch?
//
// A Wayland client declares width, height, stride and offset as four
// independent integers, and nothing upstream cross-checks them against each
// other in the units that matter:
//
//   * `wl_shm_pool.create_buffer` is validated by libwayland as `stride < width`
//     — bytes compared against pixels. libwayland has no idea a format is four
//     bytes per pixel, so an ARGB8888 buffer with `stride == width` is accepted
//     and arrives here looking valid while every row is a quarter the length
//     the pixel loop will walk.
//   * `zwp_linux_buffer_params_v1.add` takes a stride with no validation at all.
//
// Every CPU-side pixel loop in this library indexes `row[x * 4 + 3]`, so a
// short stride walks off the end of the mapping. Reading past it leaks adjacent
// process memory back to a client through screencopy; writing past it — which
// is what the capture protocols do into a client's buffer — is memory
// corruption. Both are reachable by an unprivileged client.
//
// So: nothing in this library may index into client memory without first
// passing the layout through `layout_fits()`. Validate at the protocol request
// where a proper error can be posted, AND again at the pixel loop, so a future
// caller cannot reintroduce the hole by taking a shortcut.

module;

#include <cstddef>
#include <cstdint>

export module luminaria:pixel_layout;

import std;
export namespace luminaria {

/// Every format this library touches on the CPU (ARGB8888 / XRGB8888) is four
/// bytes per pixel. The pixel loops hardcode that; this names it.
inline constexpr std::int64_t kBytesPerPixel = 4;

/// The tightest stride a row of `width` pixels can have, or -1 if `width` is
/// not a sane pixel count. Never overflows: width is bounded first.
[[nodiscard]] constexpr std::int64_t min_stride(int width) noexcept {
    // 1<<24 pixels is ~67 MB per row — far past any real display, and small
    // enough that width * 4 * height cannot overflow int64 for a sane height.
    constexpr int kMaxDimension = 1 << 24;
    if (width <= 0 || width > kMaxDimension) {
        return -1;
    }
    return static_cast<std::int64_t>(width) * kBytesPerPixel;
}

/// True if `width`/`height`/`stride` describe a layout whose pixel loop stays
/// inside a mapping of `stride * height` bytes starting at `offset`.
///
/// This is the whole check: given `stride >= width * 4`, the last byte touched
/// is `(height-1) * stride + width * 4 - 1 <= height * stride - 1`, which is
/// the last byte of the mapping. A caller that mapped exactly
/// `offset + stride * height` bytes (dmabuf), or whose provider guarantees
/// `offset + stride * height <= capacity` (libwayland does this for wl_shm), is
/// then safe to walk row by row.
[[nodiscard]] constexpr bool layout_fits(int width, int height, std::int64_t stride,
                                         std::int64_t offset = 0) noexcept {
    constexpr int kMaxDimension = 1 << 24;
    const std::int64_t needed = min_stride(width);
    if (needed < 0 || height <= 0 || height > kMaxDimension) {
        return false;
    }
    if (offset < 0 || stride < needed) {
        return false;
    }
    // Guard the size computation itself: callers turn it into a mmap length.
    const std::int64_t rows = static_cast<std::int64_t>(height);
    if (stride > std::numeric_limits<std::int64_t>::max() / rows) {
        return false;
    }
    return offset <= std::numeric_limits<std::int64_t>::max() - stride * rows;
}

/// Byte length a caller must map to walk `height` rows of `stride` at `offset`.
/// Only meaningful once `layout_fits()` has said yes; returns 0 otherwise, so a
/// caller that skips the check maps nothing rather than something wrong.
[[nodiscard]] constexpr std::size_t layout_length(int width, int height, std::int64_t stride,
                                                  std::int64_t offset = 0) noexcept {
    if (!layout_fits(width, height, stride, offset)) {
        return 0;
    }
    return static_cast<std::size_t>(offset + stride * static_cast<std::int64_t>(height));
}

} // namespace luminaria
