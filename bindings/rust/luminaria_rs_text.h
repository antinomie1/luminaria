// luminaria_rs_text.h — plain header for the luminaria.text Rust binding bridge.
#pragma once

#include <cstdint>
#include <memory>

#include "rust/cxx.h"

namespace lum {

class FontShim {
public:
    ~FontShim();
    FontShim(const FontShim&) = delete;
    FontShim& operator=(const FontShim&) = delete;

    std::int32_t height() const;
    std::int32_t ascent() const;
    std::int32_t measure(rust::Str utf8) const;
    std::int32_t draw(rust::Slice<std::uint8_t> canvas_rgba, std::int32_t width, std::int32_t height,
                      std::int32_t x, std::int32_t baseline, rust::Str utf8,
                      float r, float g, float b, float a, std::int32_t right_edge) const;

    struct Impl;
    Impl& impl() const;
    explicit FontShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<FontShim> font_open(rust::Str pattern);
rust::Vec<std::uint32_t> text_decode_utf8(rust::Str utf8);

} // namespace lum
