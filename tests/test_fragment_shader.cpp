// A custom fragment stage is a real Vulkan pipeline, not a decorative API:
// it changes a red texture to green, and its damage declaration keeps a frame
// alive even when the placement list and client damage are otherwise unchanged.
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "tint_frag_spv.h"

import luminaria.gpu;
import std;

namespace {
constexpr std::uint32_t kXrgb8888 = 0x34325258u;
}

int main() {
    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77;
    }
    const std::uint32_t not_spirv[] = {0};
    assert(!renderer->create_fragment_shader(not_spirv, luminaria::ShaderDamage::none));
    auto shader = renderer->create_fragment_shader(kTintFragSpv, luminaria::ShaderDamage::full);
    assert(shader.has_value());

    std::vector<std::uint8_t> red(4 * 4 * 4, 0);
    for (std::size_t i = 0; i < red.size(); i += 4) {
        red[i] = 255;
        red[i + 3] = 255;
    }
    auto texture = renderer->upload_texture(4, 4, red);
    assert(texture.has_value());

    auto display = luminaria::Display::create();
    assert(display.has_value());
    luminaria::HeadlessOutput output(display->event_loop(), 16, 12, 1);
    luminaria::Frame frame(output, *renderer);
    assert(frame.reset(kXrgb8888).has_value());

    const auto transform = luminaria::PlacementTransform::at(2, 3, 4, 4);
    frame.begin({0, 0, 16, 12});
    frame.place(*texture, transform, *shader);
    assert(frame.submit(luminaria::Color{0, 0, 0, 1}) == luminaria::Presented::composited);
    const auto& pixels = output.last_frame();
    const luminaria::Pixel changed = pixels[static_cast<std::size_t>(3) * 16 + 2];
    assert((changed == luminaria::Pixel{0, 255, 0, 255}));

    // No client buffer changed and the placement is identical, but full means
    // the effect owns a value Frame cannot see: do not return unchanged.
    frame.begin({0, 0, 16, 12});
    frame.place(*texture, transform, *shader);
    assert(frame.submit(luminaria::Color{0, 0, 0, 1}) == luminaria::Presented::composited);
    return 0;
}
