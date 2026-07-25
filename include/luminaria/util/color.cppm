// luminaria/util/color.cppm — straight-alpha linear RGBA, [0,1].

module;

export module luminaria:util.color;

export namespace luminaria {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    [[nodiscard]] constexpr bool operator==(const Color&) const noexcept = default;
};

} // namespace luminaria
