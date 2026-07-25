// Phase 1 slice B: Vulkan renders a solid frame offscreen; we read it back and
// verify the color. Skips (exit 77) if no Vulkan device is present.
#include <cassert>
#include <cstdio>

import luminaria;

int main() {
    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77; // meson: test skipped
    }

    auto px = renderer->render_clear_readback(64, 64, luminaria::Color{1.0f, 0.0f, 0.0f, 1.0f});
    assert(px.has_value());
    assert((*px == luminaria::Pixel{255, 0, 0, 255}));

    auto green = renderer->render_clear_readback(16, 16, luminaria::Color{0.0f, 1.0f, 0.0f, 1.0f});
    assert(green.has_value());
    assert((*green == luminaria::Pixel{0, 255, 0, 255}));

    return 0;
}
