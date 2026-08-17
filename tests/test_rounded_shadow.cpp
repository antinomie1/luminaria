// Rounded corners and drop shadows, both analytic.
//
// Neither one gets a blur pass or a second render target: the shape is a signed
// distance to a rounded box, evaluated in the fragment shader of the very quad
// that was going to be drawn anyway. So the two things worth asserting are that
// the mask actually cuts (a corner of a rounded window is see-through, not just
// darker) and that a shadow falls *outside* its caster and nowhere else — a
// window darkened by its own shadow is the bug this shape exists to avoid.
//
// Skips (77) without a Vulkan device.
#include <cassert>
#include <cstdint>
#include <cstdio>

#include <drm_fourcc.h>

import luminaria.gpu;
import std;

namespace {

constexpr int kW = 64, kH = 64;

std::vector<std::uint8_t> opaque_white(int w, int h) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4, 0xFF);
}

// White on black on an XRGB target: every channel carries the same value, so
// one of them is the coverage the shader computed.
int value_at(const std::vector<std::uint8_t>& rgba, int x, int y) {
    return rgba[(static_cast<std::size_t>(y) * kW + x) * 4];
}

bool near(int value, int want, int tolerance = 8) {
    return value >= want - tolerance && value <= want + tolerance;
}

} // namespace

int main() {
    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77;
    }
    auto target = renderer->create_scanout(kW, kH, DRM_FORMAT_XRGB8888, {});
    if (!target) {
        std::fprintf(stderr, "skip: %s\n", target.error().message.c_str());
        return 77;
    }

    const luminaria::Color black{0, 0, 0, 1};
    std::vector<std::uint8_t> pixels;

    // --- a rounded window ----------------------------------------------------
    {
        auto texture = renderer->upload_texture(32, 32, opaque_white(32, 32));
        assert(texture.has_value());

        luminaria::GpuTextureFill fill{};
        fill.texture = &*texture;
        fill.x = 16;
        fill.y = 16;
        fill.w = 32;
        fill.h = 32;
        fill.corner_radius = 12.0f;
        const luminaria::GpuTextureFill fills[] = {fill};

        assert(renderer->render_to(*target, black, {}, fills).has_value());
        assert(renderer->read_scanout(*target, pixels).has_value());

        // The middle is the client's pixels, untouched.
        assert(near(value_at(pixels, 32, 32), 255));
        // The top-left corner of the box is outside a radius-12 quarter circle
        // centred at (28,28): it must be background, not a dimmed white.
        assert(near(value_at(pixels, 17, 17), 0));
        // ... while the edge midpoints, which the mask does not touch, are not.
        assert(near(value_at(pixels, 32, 17), 255));
        assert(near(value_at(pixels, 17, 32), 255));
    }

    // --- a drop shadow -------------------------------------------------------
    {
        // A 16x16 caster at (24,24) with an 8px falloff. Solid: a shadow has no
        // texture, it is the same quad the colour would have been drawn with.
        luminaria::GpuTextureFill shadow{};
        shadow.solid = true;
        shadow.color = luminaria::Color{1, 1, 1, 1};
        shadow.x = 24;
        shadow.y = 24;
        shadow.w = 16;
        shadow.h = 16;
        shadow.feather = 8.0f;
        const luminaria::GpuTextureFill fills[] = {shadow};

        assert(renderer->render_to(*target, black, {}, fills).has_value());
        assert(renderer->read_scanout(*target, pixels).has_value());

        // Nothing inside the caster: the window drawn on top of it would
        // otherwise be sitting on a bright square it cannot see.
        assert(near(value_at(pixels, 32, 32), 0));
        // Just outside the edge, almost the full shadow colour.
        assert(value_at(pixels, 32, 22) > 200);
        // Halfway through the falloff, roughly half of it. smoothstep is not
        // linear, so this is the loosest assertion in the file.
        assert(near(value_at(pixels, 32, 20), 128, 40));
        // And past the falloff, nothing — a shadow that reaches further than it
        // says would be repainting outside the damage its caller reported.
        assert(near(value_at(pixels, 32, 12), 0));
        // The quad is grown by the falloff on every side, so the corner of the
        // grown box is covered but far enough away to be dark.
        assert(near(value_at(pixels, 17, 17), 0));
    }

    return 0;
}
