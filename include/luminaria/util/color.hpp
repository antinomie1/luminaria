// luminaria/util/color.hpp — straight-alpha linear RGBA, [0,1].
#pragma once

namespace luminaria {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    friend constexpr bool operator==(const Color&, const Color&) = default;
};

} // namespace luminaria
