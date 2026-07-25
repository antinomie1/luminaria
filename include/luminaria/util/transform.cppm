// luminaria/util/transform.cppm — output rotation and reflection.
//
// A monitor can be mounted sideways, and a HiDPI one packs more pixels into the
// same physical area. Both mean the compositor's *logical* coordinate space —
// where windows live and the pointer moves — is not the framebuffer's pixel
// grid. This header is the whole of that mapping.
//
// Values match `WL_OUTPUT_TRANSFORM_*` exactly, so they go straight into
// wl_output.geometry. The meaning here is the compositor's: `transform` is what
// we apply to logical content to land it on the framebuffer.

module;

#include <cstdint>

export module luminaria:util.transform;

import :util.box;

export namespace luminaria {

enum class Transform : std::uint8_t {
    normal = 0,
    rotate_90 = 1,
    rotate_180 = 2,
    rotate_270 = 3,
    flipped = 4,
    flipped_90 = 5,
    flipped_180 = 6,
    flipped_270 = 7,
};

/// True for the transforms that turn a landscape framebuffer into a portrait
/// logical space (and back) — the ones that swap width and height.
[[nodiscard]] constexpr bool transform_swaps_axes(Transform t) noexcept {
    const auto v = static_cast<std::uint8_t>(t);
    return (v & 1u) != 0;
}

/// True if the transform mirrors along the logical x axis before rotating.
[[nodiscard]] constexpr bool transform_flipped(Transform t) noexcept {
    return static_cast<std::uint8_t>(t) >= 4;
}

/// Rotation part only, in degrees clockwise: 0, 90, 180 or 270.
[[nodiscard]] constexpr int transform_rotation(Transform t) noexcept {
    return (static_cast<std::uint8_t>(t) & 3u) * 90;
}

/// The transform that undoes `t`. Use it to map device pixels (a pointer
/// position reported by the hardware) back into logical coordinates.
[[nodiscard]] constexpr Transform transform_invert(Transform t) noexcept {
    if (transform_flipped(t)) {
        return t; // a flip composed with a rotation is its own inverse
    }
    switch (t) {
    case Transform::rotate_90:
        return Transform::rotate_270;
    case Transform::rotate_270:
        return Transform::rotate_90;
    default:
        return t;
    }
}

/// `a` applied after `b`. Used to fold a client's wl_surface.set_buffer_transform
/// into the output's own rotation, so the renderer still does one sample.
/// (A rotation applied to an already-flipped space turns the other way, which is
/// why the flipped case subtracts.)
[[nodiscard]] constexpr Transform transform_compose(Transform a, Transform b) noexcept {
    const auto va = static_cast<std::uint8_t>(a);
    const auto vb = static_cast<std::uint8_t>(b);
    const std::uint8_t flip = (va & 4u) ^ (vb & 4u);
    const std::uint8_t rot =
        static_cast<std::uint8_t>(((vb & 4u) != 0 ? (va - vb) : (va + vb)) & 3u);
    return static_cast<Transform>(flip | rot);
}

/// Map a box from logical coordinates onto a `device_w`x`device_h` framebuffer.
/// `scale` is the integer output scale: 2 means one logical unit is 2 pixels.
///
/// The logical space is `device_w/scale` x `device_h/scale`, with width and
/// height swapped for the rotating transforms.
[[nodiscard]] constexpr Box transform_box(Transform t, int scale, Box logical, int device_w,
                                          int device_h) noexcept {
    if (scale < 1) {
        scale = 1;
    }
    if (transform_flipped(t)) {
        const int logical_w = (transform_swaps_axes(t) ? device_h : device_w) / scale;
        logical.x = logical_w - (logical.x + logical.width);
    }
    const Box s{logical.x * scale, logical.y * scale, logical.width * scale,
                logical.height * scale};
    switch (transform_rotation(t)) {
    case 90:
        return Box{device_w - (s.y + s.height), s.x, s.height, s.width};
    case 180:
        return Box{device_w - (s.x + s.width), device_h - (s.y + s.height), s.width, s.height};
    case 270:
        return Box{s.y, device_h - (s.x + s.width), s.height, s.width};
    default:
        return s;
    }
}

} // namespace luminaria
