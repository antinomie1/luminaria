// luminaria/util/blur.cppm — how much a blur blurs, and how far it reaches.
//
// A value type, so it lives below both users: `VulkanRenderer` builds its
// Kawase chain from it (`luminaria.render`) and `SceneBlur` carries it into the
// scene list (`luminaria.shell`). Putting it in either of those would have made
// the other import across a layer for four numbers.
export module luminaria:blur;

import std;

export namespace luminaria {

/// How much a blur blurs.
///
/// The names are niri's rather than Hyprland's single `size`, because the two
/// knobs do not cost the same and one number hides which one is being turned.
/// `passes` buys radius by halving the resolution again — each one is another
/// pair of render passes — while `offset` buys radius inside the passes already
/// paid for and is free. Reach for `offset` first; add a pass when the samples
/// start to separate into a visible cross.
struct BlurParams {
    /// Down/up levels. Clamped to what the chain was built for.
    int passes = 3;
    /// Tap distance, in half source texels. Above ~4 the five taps stop
    /// overlapping and the blur turns into a star.
    float offset = 2.0f;
    /// Dither amplitude on the final upsample, 0 for none. A large smooth
    /// gradient banded into 8-bit is the artefact this exists for.
    float noise = 0.0f;
    /// 1.0 leaves colour alone. Above it, undoes the averaging-towards-grey a
    /// blur necessarily does.
    float saturation = 1.0f;

    [[nodiscard]] constexpr bool operator==(const BlurParams&) const noexcept = default;
};

/// How far a blurred pixel reaches for its input, in source pixels.
///
/// Damage, not decoration: a blurred window shows what is behind it from this
/// far outside itself, so a change anywhere within this distance of it makes it
/// stale. Deliberately generous — repainting a little too much is a cost, and
/// repainting too little is a smear that stays on the screen.
[[nodiscard]] inline int blur_spread(const BlurParams& params) noexcept {
    const int passes = std::clamp(params.passes, 1, 6);
    return static_cast<int>(
        std::ceil(2.0f * std::max(params.offset, 0.0f) * static_cast<float>(1 << passes)));
}

} // namespace luminaria
