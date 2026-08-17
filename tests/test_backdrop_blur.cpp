// Blur whose source is the scene, not the wallpaper.
//
// This is the one thing `place_xray_blur` cannot do and the reason the
// expensive path exists: what is behind a translucent window is a *window*.
// So the frame below the blur is two hard-edged halves — red and blue — and the
// assertion is that inside the blurred region the two have mixed, while outside
// it the edge is still exactly where it was.
//
// It also pins the damage rule. A blurred region reads pixels from `spread`
// away, so a change that far outside it makes it stale; a frame with no change
// at all must still cost nothing, which is the ADR 0002 property.
//
// Skips (77) without a Vulkan device.
#include <cassert>
#include <cstdint>
#include <cstdio>

import luminaria.gpu;
import std;

namespace {

constexpr std::uint32_t kXrgb8888 = 0x34325258u;
constexpr int kW = 64, kH = 64;
constexpr int kEdge = 32;
// The blurred window sits over the edge, so its own area sees both colours.
constexpr luminaria::Box kBlurBox{16, 8, 32, 48};

const luminaria::Pixel& at(const std::vector<luminaria::Pixel>& px, int x, int y) {
    return px[static_cast<std::size_t>(y) * kW + x];
}

} // namespace

int main() {
    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77;
    }
    auto display = luminaria::Display::create();
    assert(display.has_value());
    luminaria::HeadlessOutput output(display->event_loop(), kW, kH, 1);
    luminaria::Frame frame(output, *renderer);
    if (auto ready = frame.reset(kXrgb8888); !ready) {
        std::fprintf(stderr, "skip: %s\n", ready.error().message.c_str());
        return 77;
    }
    auto backdrop = renderer->create_offscreen(kW, kH);
    auto chain = renderer->create_blur_chain(kW, kH, 4);
    if (!backdrop || !chain) {
        std::fprintf(stderr, "skip: no offscreen/blur targets\n");
        return 77;
    }

    const luminaria::BlurParams params{.passes = 2, .offset = 2.0f};
    const int spread = luminaria::blur_spread(params);
    assert(spread > 0);

    const luminaria::Box view{0, 0, kW, kH};
    const luminaria::Color black{0, 0, 0, 1};

    // Two opaque halves, then a queued blur backdrop of everything below them.
    // The capture itself is deferred: it runs inside submit(), and only when
    // the frame actually repaints.
    const auto draw = [&](int edge) {
        frame.begin(view);
        const luminaria::PlacementGroup below = frame.begin_group();
        frame.place_rect(0, 0, edge, kH, luminaria::Color{1, 0, 0, 1});
        frame.place_rect(edge, 0, kW - edge, kH, luminaria::Color{0, 0, 1, 1});
        assert(frame.queue_blur(below, *backdrop, *chain, view, params, kBlurBox, 0.0f, spread));
        return frame.submit(black);
    };

    assert(draw(kEdge) == luminaria::Presented::composited);
    const std::uint64_t submitted = renderer->submission_count();
    const std::vector<luminaria::Pixel>& px = output.last_frame();

    // Outside the blurred region the two halves are untouched: the backdrop is
    // a placement like any other and does not repaint the whole output.
    assert(at(px, 4, 32).r > 200 && at(px, 4, 32).b < 40);
    assert(at(px, 60, 32).b > 200 && at(px, 60, 32).r < 40);

    // Inside it, on the blue side of the edge, red has bled across — which it
    // could only do by sampling the rectangle below. This assertion is the
    // difference between this file and test_frame_xray_blur.cpp.
    const luminaria::Pixel mixed = at(px, kEdge + 3, 32);
    assert(mixed.r > 30 && mixed.b > 30);
    // ... and symmetrically on the red side.
    const luminaria::Pixel other = at(px, kEdge - 3, 32);
    assert(other.r > 30 && other.b > 30);

    // A second identical frame changes nothing, and the blur must not be what
    // makes an idle desktop repaint forever: it returns `unchanged` AND makes
    // no GPU submissions at all — the deferred capture never ran.
    assert(draw(kEdge) == luminaria::Presented::unchanged);
    assert(renderer->submission_count() == submitted);

    // Damage below the blur: the backdrop moves under it, so the capture has
    // to run again and the pixels inside the region have to change with it.
    // Both the submission count and the output are proof.
    assert(draw(kEdge + 8) == luminaria::Presented::composited);
    assert(renderer->submission_count() > submitted);
    const luminaria::Pixel moved = at(output.last_frame(), kEdge + 3, 32);
    assert(!(moved == mixed));

    return 0;
}
