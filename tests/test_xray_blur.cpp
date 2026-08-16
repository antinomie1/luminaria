// X-ray blur keeps one reduced-resolution static backdrop. Upscaling that
// cache must soften a hard edge instead of sampling the source image verbatim.
#include <cassert>
#include <cstdint>
#include <cstdio>

import luminaria.gpu;
import std;

namespace {
constexpr int kWidth = 16;
constexpr int kHeight = 8;
constexpr std::uint32_t kXrgb8888 = 0x34325258u; // DRM_FORMAT_XRGB8888
}

int main() {
    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4, 255);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth / 2; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y * kWidth + x) * 4;
            pixels[pixel] = 0;
            pixels[pixel + 1] = 0;
            pixels[pixel + 2] = 0;
        }
    }
    auto source = renderer->upload_texture(kWidth, kHeight, pixels);
    assert(source.has_value());
    auto blur = renderer->create_xray_blur(kWidth, kHeight, 4);
    assert(blur.has_value());
    assert(blur->output_width() == kWidth && blur->output_height() == kHeight);
    assert(blur->downsample() == 4);
    assert(blur->texture().width() == 4 && blur->texture().height() == 2);
    const luminaria::GpuTextureFill backdrop{&*source, 0, 0, kWidth, kHeight};
    assert(renderer->update_xray_blur(*blur, luminaria::Color{0, 0, 0, 1}, {&backdrop, 1})
               .has_value());

    auto display = luminaria::Display::create();
    assert(display.has_value());
    luminaria::HeadlessOutput output(display->event_loop(), kWidth, kHeight, 1);
    luminaria::Frame frame(output, *renderer);
    assert(frame.reset(kXrgb8888).has_value());
    frame.begin({0, 0, kWidth, kHeight});
    frame.place(blur->texture(), 0, 0, kWidth, kHeight);
    assert(frame.submit(luminaria::Color{0, 0, 0, 1}).has_value());
    const std::vector<luminaria::Pixel>& shown = output.last_frame();
    assert(shown.size() == static_cast<std::size_t>(kWidth) * kHeight);
    // The source has only 0 and 255. Linear expansion of the reduced cache
    // must make an edge pixel gray; otherwise we accidentally drew the source.
    const luminaria::Pixel edge = shown[static_cast<std::size_t>(kHeight / 2) * kWidth + 8];
    assert(edge.r > 0 && edge.r < 255);
    assert(edge.r == edge.g && edge.g == edge.b && edge.a == 255);
    return 0;
}
