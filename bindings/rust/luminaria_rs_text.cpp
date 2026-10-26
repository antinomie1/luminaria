// luminaria_rs_text.cpp — C++ shim for luminaria.text (fcft + pixman).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "luminaria/luminaria_rs_text.h"
#include "luminaria/src/ffi_text.rs.h"

import luminaria.text;

namespace lum {

namespace lm = luminaria;

struct FontShim::Impl {
    std::optional<lm::Font> font;
};

FontShim::~FontShim() = default;
FontShim::FontShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
FontShim::Impl& FontShim::impl() const { return *impl_; }

std::unique_ptr<FontShim> font_open(rust::Str pattern) {
    auto f = lm::Font::open(std::string_view(pattern.data(), pattern.size()));
    if (!f.has_value()) return nullptr;
    auto impl = std::make_unique<FontShim::Impl>();
    impl->font.emplace(std::move(*f));
    return std::make_unique<FontShim>(std::move(impl));
}

std::int32_t FontShim::height() const {
    return impl_->font ? impl_->font->height() : 0;
}

std::int32_t FontShim::ascent() const {
    return impl_->font ? impl_->font->ascent() : 0;
}

std::int32_t FontShim::measure(rust::Str utf8) const {
    return impl_->font ? impl_->font->measure(std::string_view(utf8.data(), utf8.size())) : 0;
}

std::int32_t FontShim::draw(rust::Slice<std::uint8_t> canvas_rgba, std::int32_t width, std::int32_t height,
                            std::int32_t x, std::int32_t baseline, rust::Str utf8,
                            float r, float g, float b, float a, std::int32_t right_edge) const {
    if (!impl_->font || width <= 0 || height <= 0) return x;
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (canvas_rgba.size() < pixels * sizeof(lm::Pixel)) return x;
    std::span<lm::Pixel> span(reinterpret_cast<lm::Pixel*>(canvas_rgba.data()), pixels);
    return impl_->font->draw(span, width, height, x, baseline,
                             std::string_view(utf8.data(), utf8.size()),
                             lm::Color{r, g, b, a}, right_edge);
}

rust::Vec<std::uint32_t> text_decode_utf8(rust::Str utf8) {
    rust::Vec<std::uint32_t> out;
    for (char32_t cp : lm::decode_utf8(std::string_view(utf8.data(), utf8.size()))) {
        out.push_back(static_cast<std::uint32_t>(cp));
    }
    return out;
}

} // namespace lum
