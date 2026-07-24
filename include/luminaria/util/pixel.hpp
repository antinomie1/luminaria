// luminaria/util/pixel.hpp — one RGBA8 pixel. The currency between the renderer's
// read-back and an output's scanout buffer.
#pragma once

#include <cstdint>

namespace luminaria {

struct Pixel {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
    friend constexpr bool operator==(const Pixel&, const Pixel&) = default;
};

} // namespace luminaria
