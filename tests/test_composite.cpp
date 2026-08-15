// Solid-colour compositing on the GPU: hand the renderer a back-to-front list
// of RectFills, read the frame back, and verify the pixels land where expected.
// Skips (exit 77) if no Vulkan device.
#include <cassert>
#include <cstdio>
#include <vector>

import luminaria;

using namespace luminaria;

namespace {
Pixel at(const std::vector<Pixel>& px, int w, int x, int y) {
    return px[static_cast<size_t>(y) * w + x];
}
} // namespace

int main() {
    auto renderer = VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77;
    }

    // Full-output blue background, green square at (16,16,16x16), in that order.
    const std::vector<RectFill> rects = {
        RectFill{Box{0, 0, 64, 64}, Color{0, 0, 1, 1}},
        RectFill{Box{16, 16, 16, 16}, Color{0, 1, 0, 1}},
    };

    auto frame = renderer->render_rects(64, 64, Color{0, 0, 0, 1}, rects);
    assert(frame.has_value());
    const auto& px = *frame;

    // Corner is background blue; center of the square is green; just outside is blue.
    assert((at(px, 64, 0, 0) == Pixel{0, 0, 255, 255}));
    assert((at(px, 64, 24, 24) == Pixel{0, 255, 0, 255}));
    assert((at(px, 64, 8, 8) == Pixel{0, 0, 255, 255}));
    assert((at(px, 64, 40, 40) == Pixel{0, 0, 255, 255}));

    return 0;
}
