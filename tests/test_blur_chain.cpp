// Dual Kawase, as an actual blur rather than an API that returns success.
//
// The source is a hard black/white edge, which is the one input where "did it
// blur" has an answer that is not a matter of opinion: a step becomes a ramp,
// the midpoint of the ramp sits on the edge, and adding a pass makes the ramp
// wider. All three are asserted, plus the saturation knob, which is the only
// part of the chain that is not a weighted average.
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
constexpr int kEdge = 32; // black left of it, white right of it

std::vector<std::uint8_t> step_edge() {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kW) * kH * 4, 0);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kW + x) * 4;
            const std::uint8_t v = x < kEdge ? 0 : 255;
            rgba[i] = rgba[i + 1] = rgba[i + 2] = v;
            rgba[i + 3] = 255;
        }
    }
    return rgba;
}

std::vector<std::uint8_t> flat(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kW) * kH * 4, 0);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = 255;
    }
    return rgba;
}

int channel_at(const std::vector<std::uint8_t>& rgba, int x, int y, int c) {
    return rgba[(static_cast<std::size_t>(y) * kW + x) * 4 + static_cast<std::size_t>(c)];
}

bool near(int value, int want, int tolerance) {
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
    auto chain = renderer->create_blur_chain(kW, kH, 4);
    if (!chain) {
        std::fprintf(stderr, "skip: %s\n", chain.error().message.c_str());
        return 77;
    }
    assert(chain->width() == kW && chain->height() == kH);
    assert(chain->max_passes() == 4);

    std::vector<std::uint8_t> pixels;
    // Draw whatever the chain last produced onto the scanout, 1:1, and read it.
    const auto readback = [&](const luminaria::GpuTexture& source) {
        luminaria::GpuTextureFill fill{};
        fill.texture = &source;
        fill.w = kW;
        fill.h = kH;
        const luminaria::GpuTextureFill fills[] = {fill};
        assert(renderer->render_to(*target, luminaria::Color{0, 0, 0, 1}, {}, fills).has_value());
        assert(renderer->read_scanout(*target, pixels).has_value());
    };

    auto edge = renderer->upload_texture(kW, kH, step_edge());
    assert(edge.has_value());

    // --- one pass ------------------------------------------------------------
    assert(renderer->blur(*chain, *edge, {.passes = 1, .offset = 2.0f}).has_value());
    readback(chain->texture());
    const int narrow = channel_at(pixels, kEdge - 6, kH / 2, 0);
    // The step is still a step at the midpoint, whatever the radius: the kernel
    // is symmetric, so half of white and half of black meet there.
    assert(near(channel_at(pixels, kEdge, kH / 2, 0), 128, 24));
    // Far from the edge nothing has moved.
    assert(near(channel_at(pixels, 2, kH / 2, 0), 0, 12));
    assert(near(channel_at(pixels, kW - 3, kH / 2, 0), 255, 12));

    // --- three passes --------------------------------------------------------
    assert(renderer->blur(*chain, *edge, {.passes = 3, .offset = 2.0f}).has_value());
    readback(chain->texture());
    const int wide = channel_at(pixels, kEdge - 6, kH / 2, 0);
    assert(near(channel_at(pixels, kEdge, kH / 2, 0), 128, 24));
    // Six pixels into the black side, three passes have carried noticeably more
    // white across than one did. This is the assertion that `passes` is a knob
    // and not a number the chain accepts and ignores.
    assert(wide > narrow + 20);

    // --- saturation ----------------------------------------------------------
    // A flat colour is its own blur, so anything that changes here is the only
    // stage that is not an average.
    auto red = renderer->upload_texture(kW, kH, flat(220, 40, 40));
    assert(red.has_value());
    assert(renderer->blur(*chain, *red, {.passes = 2, .saturation = 0.0f}).has_value());
    readback(chain->texture());
    const int r = channel_at(pixels, kW / 2, kH / 2, 0);
    const int g = channel_at(pixels, kW / 2, kH / 2, 1);
    const int b = channel_at(pixels, kW / 2, kH / 2, 2);
    assert(near(r, g, 6) && near(g, b, 6));

    // ... and left alone at 1.0, which is what makes it safe as a default.
    assert(renderer->blur(*chain, *red, {.passes = 2}).has_value());
    readback(chain->texture());
    assert(near(channel_at(pixels, kW / 2, kH / 2, 0), 220, 8));
    assert(near(channel_at(pixels, kW / 2, kH / 2, 1), 40, 8));

    return 0;
}
