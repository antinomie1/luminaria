// luminaria/util/pixel.cppm — one RGBA8 pixel. The currency between the renderer's
// read-back and an output's scanout buffer.

module;

#include <cstdint>

export module luminaria:pixel;
export namespace luminaria {

struct Pixel {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
    [[nodiscard]] constexpr bool operator==(const Pixel&) const noexcept = default;
};

} // namespace luminaria
