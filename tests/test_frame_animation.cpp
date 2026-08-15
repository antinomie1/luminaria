// Frame owns the continuation of a compositor-owned animation: each animated
// build repaints and buys exactly one more paced frame; omitting animate() on
// the final build returns immediately to the unchanged/idle path.
#include <cassert>
#include <cstdint>
#include <cstdio>

import luminaria.gpu;

namespace {
// DRM_FORMAT_XRGB8888. Keep this test on the public C++ module surface rather
// than making dependency scanning find libdrm's private include directory.
constexpr std::uint32_t kXrgb8888 = 0x34325258u;
}

int main() {
    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77;
    }
    auto display = luminaria::Display::create();
    assert(display.has_value());
    luminaria::HeadlessOutput output(display->event_loop(), 32, 24, 1);
    luminaria::Frame frame(output, *renderer);
    assert(frame.reset(kXrgb8888).has_value());

    int ticks = 0;
    auto frames = output.frame.connect([&](luminaria::FrameEvent& event) {
        ++ticks;
        assert(event.predicted_presentation_ns != 0);
        assert(event.refresh_ns == 1000000);
        assert(!event.hw_clock);
        frame.begin({0, 0, 32, 24});
        if (ticks < 3) {
            frame.animate();
        }
        auto result = frame.submit(luminaria::Color{0, 0, 0, 1});
        assert(result.has_value());
        if (ticks < 3) {
            assert(*result == luminaria::Presented::composited);
            assert(output.frame_scheduled());
        } else {
            assert(*result == luminaria::Presented::unchanged);
            assert(!output.frame_scheduled());
            display->terminate();
        }
    });
    display->run();
    assert(ticks == 3);
    return 0;
}
