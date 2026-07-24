// Step 1: scene -> Vulkan compositing. Flatten a scene to RectFills, composite
// them on the GPU, read the frame back, and verify pixels land where expected.
// Skips (exit 77) if no Vulkan device.
#include <cassert>
#include <cstdio>
#include <vector>

#include "luminaria/render/vulkan.hpp"
#include "luminaria/scene.hpp"

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

    // Build a scene: full-output blue background, green square at (16,16,16x16).
    Scene scene;
    SceneRect& bg = scene.root().add_rect(64, 64, Color{0, 0, 1, 1});
    (void)bg;
    SceneRect& sq = scene.root().add_rect(16, 16, Color{0, 1, 0, 1});
    sq.set_position(16, 16);

    std::vector<RectFill> rects = scene_rects(scene.root());
    assert(rects.size() == 2);

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
