// Surface texture compositing: upload a client-buffer-like RGBA texture and
// composite it over background + rects on the GPU; verify pixels. Skips if no
// Vulkan device.
#include <cassert>
#include <cstdint>
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

    // 8x8 opaque-blue "client buffer".
    std::vector<std::uint8_t> blue(8 * 8 * 4);
    for (size_t i = 0; i < blue.size(); i += 4) {
        blue[i] = 0;
        blue[i + 1] = 0;
        blue[i + 2] = 255;
        blue[i + 3] = 255;
    }

    RectFill rects[] = {{Box{16, 16, 16, 16}, Color{0, 1, 0, 1}}}; // green square
    TextureFill textures[] = {{40, 40, 8, 8, blue.data()}};        // blue at (40,40)

    auto frame = renderer->composite(64, 64, Color{1, 0, 0, 1}, rects, textures); // red bg
    assert(frame.has_value());
    const auto& px = *frame;

    assert((at(px, 64, 0, 0) == Pixel{255, 0, 0, 255}));    // background red
    assert((at(px, 64, 24, 24) == Pixel{0, 255, 0, 255}));  // rect green
    assert((at(px, 64, 44, 44) == Pixel{0, 0, 255, 255}));  // texture blue
    assert((at(px, 64, 40, 40) == Pixel{0, 0, 255, 255}));  // texture top-left
    assert((at(px, 64, 39, 39) == Pixel{255, 0, 0, 255}));  // just outside -> bg
    return 0;
}
