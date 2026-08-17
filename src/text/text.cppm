// luminaria/text/text.cppm — glyph rasterization and font metrics.
module;

#define restrict __restrict
extern "C" {
#include <fcft/fcft.h>
}
#undef restrict

#include <pixman.h>

export module luminaria.text;

import std;

import luminaria;

export namespace luminaria {

/// A rasterizable font, opened from a fontconfig pattern like `"monospace:size=11"`.
class Font {
public:
    [[nodiscard]] static std::optional<Font> open(std::string_view pattern);

    ~Font();
    Font(Font&& other) noexcept : font_(std::exchange(other.font_, nullptr)) {}
    Font& operator=(Font&& other) noexcept;
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] int ascent() const noexcept;

    [[nodiscard]] int measure(std::string_view utf8) const;

    int draw(std::span<Pixel> canvas, int width, int height, int x, int baseline,
             std::string_view utf8, Color color, int right_edge) const;

private:
    explicit Font(fcft_font* font) noexcept : font_(font) {}

    fcft_font* font_ = nullptr;
};

[[nodiscard]] std::vector<char32_t> decode_utf8(std::string_view utf8);

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {
namespace {

constexpr char32_t kReplacement = 0xfffd;

struct Library {
    Library() { fcft_init(FCFT_LOG_COLORIZE_AUTO, false, FCFT_LOG_CLASS_WARNING); }
    ~Library() { fcft_fini(); }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
};

void init_once() {
    static const Library library;
    (void)library;
}

[[nodiscard]] std::uint8_t scale8(std::uint8_t value, unsigned by) noexcept {
    return static_cast<std::uint8_t>(value * by / 255u);
}

[[nodiscard]] Pixel over(Pixel src, Pixel dst) noexcept {
    const unsigned inv = 255u - src.a;
    if (inv == 0u) {
        return src;
    }
    return {static_cast<std::uint8_t>(src.r + dst.r * inv / 255u),
            static_cast<std::uint8_t>(src.g + dst.g * inv / 255u),
            static_cast<std::uint8_t>(src.b + dst.b * inv / 255u),
            static_cast<std::uint8_t>(src.a + dst.a * inv / 255u)};
}

[[nodiscard]] Pixel to_pixel(Color color) noexcept {
    const auto byte = [](float v) {
        return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    const std::uint8_t a = byte(color.a);
    return {scale8(byte(color.r), a), scale8(byte(color.g), a), scale8(byte(color.b), a), a};
}

struct GlyphPixels {
    const std::uint8_t* data = nullptr;
    int stride = 0;
    bool argb = false;
};

[[nodiscard]] std::optional<GlyphPixels> glyph_pixels(const fcft_glyph& glyph) {
    if (glyph.pix == nullptr) {
        return std::nullopt;
    }
    const pixman_format_code_t format = pixman_image_get_format(glyph.pix);
    if (format != PIXMAN_a8 && format != PIXMAN_a8r8g8b8 && format != PIXMAN_x8r8g8b8) {
        return std::nullopt;
    }
    return GlyphPixels{reinterpret_cast<const std::uint8_t*>(pixman_image_get_data(glyph.pix)),
                       pixman_image_get_stride(glyph.pix), format != PIXMAN_a8};
}

} // namespace

std::vector<char32_t> decode_utf8(std::string_view utf8) {
    std::vector<char32_t> out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<std::uint8_t>(utf8[i]);
        const int extra = lead < 0x80 ? 0 : lead < 0xc0 ? -1 : lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
        if (extra < 0 || i + static_cast<std::size_t>(extra) >= utf8.size()) {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        char32_t cp = extra == 0 ? lead : lead & (0x7fu >> (extra + 1));
        bool valid = true;
        for (int k = 1; k <= extra; ++k) {
            const auto byte = static_cast<std::uint8_t>(utf8[i + static_cast<std::size_t>(k)]);
            if ((byte & 0xc0u) != 0x80u) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (byte & 0x3fu);
        }
        if (!valid || cp > 0x10ffff) {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += static_cast<std::size_t>(extra) + 1;
    }
    return out;
}

std::optional<Font> Font::open(std::string_view pattern) {
    init_once();
    const std::string name{pattern};
    const char* names[] = {name.c_str()};
    fcft_font* font = fcft_from_name(1, names, nullptr);
    if (font == nullptr) {
        return std::nullopt;
    }
    return Font{font};
}

Font::~Font() {
    if (font_ != nullptr) {
        fcft_destroy(font_);
    }
}

Font& Font::operator=(Font&& other) noexcept {
    if (this != &other) {
        if (font_ != nullptr) {
            fcft_destroy(font_);
        }
        font_ = std::exchange(other.font_, nullptr);
    }
    return *this;
}

int Font::height() const noexcept {
    return font_ != nullptr ? font_->height : 0;
}

int Font::ascent() const noexcept {
    return font_ != nullptr ? font_->ascent : 0;
}

int Font::measure(std::string_view utf8) const {
    if (font_ == nullptr) {
        return 0;
    }
    int width = 0;
    for (const char32_t cp : decode_utf8(utf8)) {
        const fcft_glyph* glyph = fcft_rasterize_char_utf32(font_, cp, FCFT_SUBPIXEL_NONE);
        if (glyph != nullptr) {
            width += glyph->advance.x;
        }
    }
    return width;
}

int Font::draw(std::span<Pixel> canvas, int width, int height, int x, int baseline,
               std::string_view utf8, Color color, int right_edge) const {
    if (font_ == nullptr || width <= 0 || height <= 0 ||
        canvas.size() < static_cast<std::size_t>(width) * height) {
        return x;
    }
    const Pixel ink = to_pixel(color);
    int pen = x;
    for (const char32_t cp : decode_utf8(utf8)) {
        const fcft_glyph* glyph = fcft_rasterize_char_utf32(font_, cp, FCFT_SUBPIXEL_NONE);
        if (glyph == nullptr) {
            continue;
        }
        if (pen + glyph->advance.x > right_edge) {
            break;
        }
        const std::optional<GlyphPixels> pixels = glyph_pixels(*glyph);
        if (pixels.has_value()) {
            const int left = pen + glyph->x;
            const int top = baseline - glyph->y;
            for (int row = 0; row < glyph->height; ++row) {
                const int dst_y = top + row;
                if (dst_y < 0 || dst_y >= height) {
                    continue;
                }
                const std::uint8_t* src = pixels->data + static_cast<std::ptrdiff_t>(row) *
                                                             pixels->stride;
                for (int col = 0; col < glyph->width; ++col) {
                    const int dst_x = left + col;
                    if (dst_x < 0 || dst_x >= width) {
                        continue;
                    }
                    Pixel& dst = canvas[static_cast<std::size_t>(dst_y) * width + dst_x];
                    if (pixels->argb) {
                        const std::uint8_t* bgra = src + static_cast<std::ptrdiff_t>(col) * 4;
                        dst = over({bgra[2], bgra[1], bgra[0], bgra[3]}, dst);
                    } else if (const unsigned coverage = src[col]; coverage != 0) {
                        dst = over({scale8(ink.r, coverage), scale8(ink.g, coverage),
                                    scale8(ink.b, coverage), scale8(ink.a, coverage)},
                                   dst);
                    }
                }
            }
        }
        pen += glyph->advance.x;
    }
    return pen;
}

} // namespace luminaria
