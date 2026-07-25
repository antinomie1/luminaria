// The present path: render a scene on the GPU, hand the pixels to an output via
// commit_frame, and read them back from the (headless) output. This is exactly
// what the DRM output does, minus the scanout. Skips (77) without Vulkan.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

import luminaria;

using namespace luminaria;

int main() {
    auto renderer = VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77;
    }
    auto display = Display::create();
    assert(display.has_value());

    // Scene: red background rect + green square; flatten and composite.
    Scene scene;
    scene.root().add_rect(8, 8, Color{1, 0, 0, 1});
    SceneRect& sq = scene.root().add_rect(4, 4, Color{0, 1, 0, 1});
    sq.set_position(2, 2);

    // A blue 2x2 client texture at (0,0).
    std::vector<std::uint8_t> blue(2 * 2 * 4);
    for (size_t i = 0; i < blue.size(); i += 4) {
        blue[i] = 0;
        blue[i + 1] = 0;
        blue[i + 2] = 255;
        blue[i + 3] = 255;
    }
    TextureFill tex[] = {{0, 0, 2, 2, blue.data()}};

    auto rects = scene_rects(scene.root());
    auto pixels = renderer->composite(8, 8, Color{0, 0, 0, 1}, rects, tex);
    assert(pixels.has_value());

    HeadlessOutput output(display->event_loop(), 8, 8, 16);
    assert(output.commit_frame(*pixels, 8, 8).has_value());
    // Wrong size is rejected.
    assert(!output.commit_frame(*pixels, 4, 4).has_value());

    const auto& f = output.last_frame();
    assert(f.size() == 64);
    assert((f[0 * 8 + 0] == Pixel{0, 0, 255, 255}));  // texture blue
    assert((f[3 * 8 + 3] == Pixel{0, 255, 0, 255}));  // square green
    assert((f[0 * 8 + 7] == Pixel{255, 0, 0, 255}));  // background red
    return 0;
}
