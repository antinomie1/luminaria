// Tests for luminaria.text: UTF-8 decoding, font loading, metrics, and glyph drawing.
#include <cassert>
#include <cstdio>

import luminaria.text;
import std;

using luminaria::Color;
using luminaria::Font;
using luminaria::Pixel;
using luminaria::decode_utf8;

int main() {
    // 1. UTF-8 decoding (ASCII, multi-byte, invalid sequence replacement)
    const auto cps = decode_utf8("A\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80");
    assert(cps.size() == 4);
    assert(cps[0] == U'A');
    assert(cps[1] == 0x00e9);
    assert(cps[2] == 0x4e2d);
    assert(cps[3] == 0x1f600);

    const auto bad = decode_utf8("\xff");
    assert(bad.size() == 1 && bad[0] == 0xfffd);

    // 2. Font metrics and rasterization
    std::optional<Font> font = Font::open("monospace:size=12");
    if (!font.has_value()) {
        std::fprintf(stderr, "skip: no monospace font available for fcft\n");
        return 77;
    }

    assert(font->height() > 0);
    assert(font->ascent() > 0);

    const int w = font->measure("Hello");
    assert(w > 0);
    assert(font->measure("") == 0);

    const int cw = 128;
    const int ch = 32;
    std::vector<Pixel> canvas(static_cast<std::size_t>(cw * ch), Pixel{0, 0, 0, 0});
    const int pen = font->draw(canvas, cw, ch, 4, font->ascent() + 2, "Hi",
                               Color{1.0f, 1.0f, 1.0f, 1.0f}, cw);
    assert(pen > 4);

    bool any_ink = false;
    for (const Pixel& p : canvas) {
        if (p.a != 0) {
            any_ink = true;
            break;
        }
    }
    assert(any_ink);

    return 0;
}
