// luminaria/util/color_description.cppm — what colour a surface or a display
// is in, as wp-color-management-v1 and wp-color-representation-v1 describe it.
//
// These are plain values, free of any protocol object, so `Surface` can carry
// them (they are double-buffered surface state) and a compositor can read them
// without importing the protocol glue. NOTHING HERE CONVERTS PIXELS: the
// library's renderers composite in the encoding the client gave them, which is
// right for sRGB-on-sRGB and wrong for anything else. `ImageDescription::is_srgb()`
// is how a compositor tells the two apart; the protocol layer
// (`ColorManager`) only advertises what its owner says it can honour.

module;

#include <cstdint>

export module luminaria:color_description;

import std;

export namespace luminaria {

/// Named colour primaries, with the protocol's numbering.
enum class Primaries : std::uint32_t {
    srgb = 1,
    pal_m = 2,
    pal = 3,
    ntsc = 4,
    generic_film = 5,
    bt2020 = 6,
    cie1931_xyz = 7,
    dci_p3 = 8,
    display_p3 = 9,
    adobe_rgb = 10,
};

/// Named transfer functions, with the protocol's numbering.
enum class TransferFunction : std::uint32_t {
    bt1886 = 1,
    gamma22 = 2,
    gamma28 = 3,
    st240 = 4,
    ext_linear = 5,
    log_100 = 6,
    log_316 = 7,
    xvycc = 8,
    srgb = 9,          ///< deprecated in v2: the piece-wise sRGB curve
    ext_srgb = 10,     ///< deprecated in v2
    st2084_pq = 11,    ///< HDR10's curve
    st428 = 12,
    hlg = 13,          ///< broadcast HDR
    compound_power_2_4 = 14, ///< sRGB's actual curve, as v2 names it
};

enum class RenderIntent : std::uint32_t {
    perceptual = 0,
    relative = 1,
    saturation = 2,
    absolute = 3,
    relative_bpc = 4,
    absolute_no_adaptation = 5,
};

/// CIE 1931 xy chromaticity.
struct Chromaticity {
    double x = 0.0, y = 0.0;
    [[nodiscard]] bool operator==(const Chromaticity&) const noexcept = default;
};

/// Where red, green, blue and white sit.
struct PrimarySet {
    Chromaticity red, green, blue, white;
    [[nodiscard]] bool operator==(const PrimarySet&) const noexcept = default;
};

/// The chromaticities a name stands for.
[[nodiscard]] constexpr PrimarySet primaries_of(Primaries p) noexcept {
    switch (p) {
    case Primaries::srgb:
        return {{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}, {0.3127, 0.3290}};
    case Primaries::pal_m:
        return {{0.670, 0.330}, {0.210, 0.710}, {0.140, 0.080}, {0.3100, 0.3160}};
    case Primaries::pal:
        return {{0.640, 0.330}, {0.290, 0.600}, {0.150, 0.060}, {0.3127, 0.3290}};
    case Primaries::ntsc:
        return {{0.630, 0.340}, {0.310, 0.595}, {0.155, 0.070}, {0.3127, 0.3290}};
    case Primaries::generic_film:
        return {{0.681, 0.319}, {0.243, 0.692}, {0.145, 0.049}, {0.3100, 0.3160}};
    case Primaries::bt2020:
        return {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}, {0.3127, 0.3290}};
    case Primaries::cie1931_xyz:
        return {{1.0, 0.0}, {0.0, 1.0}, {0.0, 0.0}, {1.0 / 3.0, 1.0 / 3.0}};
    case Primaries::dci_p3:
        return {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.3140, 0.3510}};
    case Primaries::display_p3:
        return {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.3127, 0.3290}};
    case Primaries::adobe_rgb:
        return {{0.640, 0.330}, {0.210, 0.710}, {0.150, 0.060}, {0.3127, 0.3290}};
    }
    return {};
}

/// A colour space as a client or a display declares it. Parametric only: ICC
/// profiles are not supported, so there is always a primary set and a transfer
/// function.
struct ImageDescription {
    PrimarySet primaries = primaries_of(Primaries::srgb);
    std::optional<Primaries> named_primaries = Primaries::srgb; ///< unset: custom chromaticities
    /// Exactly one of the two: a named curve, or a pure power `x^power`.
    std::optional<TransferFunction> transfer_function = TransferFunction::compound_power_2_4;
    double power = 0.0; ///< the exponent when `transfer_function` is unset

    /// Luminance of the primary colour volume, in cd/m² (nits).
    double min_luminance = 0.2;
    double max_luminance = 80.0;
    double reference_luminance = 80.0; ///< what diffuse white is; 203 for PQ and HLG

    /// The mastering display's volume — what the content was graded on. Only
    /// informational when the primary volume is the whole of what a display can do.
    std::optional<PrimarySet> target_primaries;
    std::optional<std::pair<double, double>> target_luminance; ///< min, max
    std::optional<std::uint32_t> max_cll;  ///< brightest pixel, cd/m²
    std::optional<std::uint32_t> max_fall; ///< brightest frame average, cd/m²

    /// The same colour space is the same `identity` across clients; the
    /// protocol hands it out so a client can tell two descriptions are equal
    /// without comparing them. 0 until the colour manager assigns one.
    std::uint64_t identity = 0;

    /// The defaults the protocol prescribes for a transfer function's luminance
    /// range when the client sets none.
    [[nodiscard]] static constexpr ImageDescription with_defaults(
        std::optional<TransferFunction> tf) noexcept {
        ImageDescription d;
        d.transfer_function = tf;
        if (tf == TransferFunction::st2084_pq) {
            d.min_luminance = 0.005;
            d.max_luminance = 10000.0;
            d.reference_luminance = 203.0;
        } else if (tf == TransferFunction::hlg) {
            d.min_luminance = 0.005;
            d.max_luminance = 1000.0;
            d.reference_luminance = 203.0;
        }
        return d;
    }

    /// Plain sRGB: the colour space nothing needs converting from.
    [[nodiscard]] static constexpr ImageDescription srgb() noexcept { return {}; }
    /// HDR10 as delivered: BT.2020 primaries, PQ, 203-nit diffuse white.
    [[nodiscard]] static constexpr ImageDescription bt2100_pq() noexcept {
        ImageDescription d = with_defaults(TransferFunction::st2084_pq);
        d.primaries = primaries_of(Primaries::bt2020);
        d.named_primaries = Primaries::bt2020;
        return d;
    }

    /// An HDR transfer function: pixels are not in the 0..1 SDR range.
    [[nodiscard]] constexpr bool is_hdr() const noexcept {
        return transfer_function == TransferFunction::st2084_pq ||
               transfer_function == TransferFunction::hlg ||
               transfer_function == TransferFunction::ext_linear;
    }
    /// sRGB primaries with an SDR curve that is, or is within a hair of, sRGB's:
    /// what the renderers already composite in, so no conversion is owed.
    [[nodiscard]] constexpr bool is_srgb() const noexcept {
        if (named_primaries != Primaries::srgb && primaries != primaries_of(Primaries::srgb)) {
            return false;
        }
        if (!transfer_function.has_value()) {
            return power >= 2.1 && power <= 2.3;
        }
        switch (*transfer_function) {
        case TransferFunction::compound_power_2_4:
        case TransferFunction::srgb:
        case TransferFunction::gamma22:
        case TransferFunction::bt1886:
            return true;
        default:
            return false;
        }
    }

    /// Everything but `identity`: two descriptions of the same space compare equal.
    [[nodiscard]] constexpr bool same_space(const ImageDescription& o) const noexcept {
        return primaries == o.primaries && transfer_function == o.transfer_function &&
               power == o.power && min_luminance == o.min_luminance &&
               max_luminance == o.max_luminance && reference_luminance == o.reference_luminance &&
               target_primaries == o.target_primaries && target_luminance == o.target_luminance &&
               max_cll == o.max_cll && max_fall == o.max_fall;
    }
};

/// wp-color-representation-v1: how a surface's pixel values are to be read
/// beyond their colour space — what the alpha channel means and, for YUV
/// content, which matrix and range turn it into RGB.
enum class AlphaMode : std::uint32_t {
    premultiplied_electrical = 0, ///< the default, and what the renderers composite
    premultiplied_optical = 1,
    straight = 2,
};
enum class Coefficients : std::uint32_t {
    identity = 1, ///< RGB, or YUV that is already RGB (GBR)
    bt709 = 2,
    fcc = 3,
    bt601 = 4,
    smpte240 = 5,
    bt2020 = 6,
    bt2020_cl = 7,
    ictcp = 8,
};
enum class ColorRange : std::uint32_t { full = 1, limited = 2 };
enum class ChromaLocation : std::uint32_t {
    type_0 = 1,
    type_1 = 2,
    type_2 = 3,
    type_3 = 4,
    type_4 = 5,
    type_5 = 6,
};

/// What a client has said about a surface's pixel representation. Unset fields
/// are the client saying nothing, which compositors read as the protocol's
/// defaults (premultiplied electrical, and no matrix to apply).
struct ColorRepresentation {
    std::optional<AlphaMode> alpha_mode;
    std::optional<Coefficients> coefficients;
    std::optional<ColorRange> range;
    std::optional<ChromaLocation> chroma_location;

    [[nodiscard]] bool operator==(const ColorRepresentation&) const noexcept = default;
};

} // namespace luminaria
