// Cursor theme loading: turning a name like "default" or "text" into real
// pixels, out of the XCursor files every desktop ships. Skips (77) on a machine
// with no cursor themes installed at all.
#include <cassert>
#include <cstdio>
#include <cstdlib>

import luminaria;
import std;

int main() {
    auto theme = luminaria::CursorTheme::load({}, 24);
    if (!theme) {
        std::fprintf(stderr, "skip: %s\n", theme.error().message.c_str());
        return 77;
    }
    assert(theme->size() == 24);

    const std::vector<luminaria::CursorImage>* arrow = theme->cursor("default");
    if (arrow == nullptr) {
        std::fprintf(stderr, "skip: theme '%s' has no 'default' cursor\n", theme->name().c_str());
        return 77;
    }
    assert(!arrow->empty());
    const luminaria::CursorImage& image = arrow->front();
    assert(image.width > 0 && image.height > 0);
    assert(image.rgba.size() == static_cast<size_t>(image.width) * image.height * 4);
    // The hotspot has to be inside the image or it points at nothing.
    assert(image.hotspot_x >= 0 && image.hotspot_x <= image.width);
    assert(image.hotspot_y >= 0 && image.hotspot_y <= image.height);
    // A cursor that is entirely transparent would be an empty screen, not a
    // pointer — the parser got the alpha channel the wrong way round if so.
    bool any_opaque = false;
    for (size_t i = 3; i < image.rgba.size(); i += 4) {
        if (image.rgba[i] != 0) {
            any_opaque = true;
            break;
        }
    }
    assert(any_opaque);
    // Premultiplied: no channel may exceed alpha.
    for (size_t i = 0; i < image.rgba.size(); i += 4) {
        const auto a = image.rgba[i + 3];
        assert(image.rgba[i + 0] <= a && image.rgba[i + 1] <= a && image.rgba[i + 2] <= a);
    }

    // The cache hands back the same object, not a re-parse.
    assert(theme->cursor("default") == arrow);
    // frame() picks something for a still cursor regardless of the time.
    assert(theme->frame("default", 0) != nullptr);
    assert(theme->frame("default", 999999) != nullptr);
    // An unknown name is a null, not a crash.
    assert(theme->cursor("no-such-cursor-name-here") == nullptr);

    // The whole point of the pairing: cursor-shape-v1 gives XDG names, and
    // those names are what the theme is keyed by. Most themes carry the common
    // ones; at least one has to resolve or the two halves don't line up.
    int resolved = 0;
    for (const char* shape : {"default", "text", "pointer", "wait", "ns-resize", "ew-resize"}) {
        if (theme->cursor(shape) != nullptr) {
            ++resolved;
        }
    }
    assert(resolved > 0);
    // cursor_shape_name() is the mapping from the protocol enum to those names.
    assert(luminaria::cursor_shape_name(1) != nullptr);
    return 0;
}
