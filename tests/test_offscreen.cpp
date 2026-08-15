// Offscreen window composition (ADR 0005), and the reason it exists.
//
// A window is a tree of surfaces. Fading it out by drawing each of them at half
// opacity is wrong wherever two of them overlap: the overlap is blended twice
// and comes out brighter than the rest, a seam that gets worse with every layer.
// That is not a subtle artefact — it is the first thing anyone sees when they
// add a fade animation, and it is why both Hyprland and niri composite a window
// offscreen before applying an effect to it.
//
// So this test does the wrong thing first and measures the seam, then does it
// through an OffscreenTarget and requires the seam to be gone. Two overlapping
// opaque quads at 50%:
//
//   direct    A alone 0.5   overlap 0.5 + 0.5*0.5 = 0.75   B alone 0.5
//   offscreen A alone 0.5   overlap             0.5        B alone 0.5
//
// Skips (77) without a Vulkan device.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <drm_fourcc.h>

import luminaria.gpu;

namespace {

constexpr int kW = 64, kH = 48;
// Two 32x32 quads overlapping in the middle third: A covers x 0..32, B covers
// x 16..48, so 16..32 is the seam and 0..16 / 32..48 are the plain halves.
constexpr int kQuadW = 32, kQuadH = 32;
constexpr int kBx = 16;
// Sample points: inside A only, inside the overlap, inside B only.
constexpr int kOnlyA = 8, kOverlap = 24, kOnlyB = 40, kRow = 8;

std::vector<std::uint8_t> opaque_white(int w, int h) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4, 0xFF);
}

// The scanout target is XRGB: the red channel is as good as any, and white is
// symmetric anyway.
int red_at(const std::vector<std::uint8_t>& rgba, int x, int y) {
    return rgba[(static_cast<std::size_t>(y) * kW + x) * 4];
}

bool near(int value, int want, int tolerance = 4) {
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
    auto tex_a = renderer->upload_texture(kQuadW, kQuadH, opaque_white(kQuadW, kQuadH));
    auto tex_b = renderer->upload_texture(kQuadW, kQuadH, opaque_white(kQuadW, kQuadH));
    assert(tex_a.has_value() && tex_b.has_value());

    const luminaria::Color black{0, 0, 0, 1};
    std::vector<std::uint8_t> pixels;

    // --- the wrong way: opacity applied per surface ---------------------------
    {
        luminaria::GpuTextureFill a{};
        a.texture = &*tex_a;
        a.x = 0;
        a.y = 0;
        a.w = kQuadW;
        a.h = kQuadH;
        a.alpha = 0.5f;
        luminaria::GpuTextureFill b = a;
        b.texture = &*tex_b;
        b.x = kBx;
        const luminaria::GpuTextureFill fills[] = {a, b};

        assert(renderer->render_to(*target, black, {}, fills).has_value());
        assert(renderer->read_scanout(*target, pixels).has_value());

        const int only_a = red_at(pixels, kOnlyA, kRow);
        const int overlap = red_at(pixels, kOverlap, kRow);
        const int only_b = red_at(pixels, kOnlyB, kRow);
        // Half of white over black.
        assert(near(only_a, 128));
        assert(near(only_b, 128));
        // And the seam: blended twice, so three quarters rather than half. This
        // assertion is the bug, asserted deliberately — if it ever stops holding,
        // the premise of ADR 0005 has changed and the rest of this test is moot.
        assert(near(overlap, 191));
        assert(overlap > only_a + 40);
    }

    // --- the right way: composite the window, then fade the result ------------
    {
        // One image for the whole window: the union of the two quads.
        const int win_w = kBx + kQuadW;
        auto offscreen = renderer->create_offscreen(win_w, kQuadH);
        if (!offscreen) {
            std::fprintf(stderr, "skip: %s\n", offscreen.error().message.c_str());
            return 77;
        }
        assert(offscreen->width() == win_w && offscreen->height() == kQuadH);

        // Into the offscreen at full opacity, over a fully transparent
        // background — what the window does not cover has to stay see-through.
        luminaria::GpuTextureFill a{};
        a.texture = &*tex_a;
        a.x = 0;
        a.y = 0;
        a.w = kQuadW;
        a.h = kQuadH;
        luminaria::GpuTextureFill b = a;
        b.texture = &*tex_b;
        b.x = kBx;
        const luminaria::GpuTextureFill window[] = {a, b};
        const luminaria::Color transparent{0, 0, 0, 0};
        assert(renderer->render_offscreen(*offscreen, transparent, {}, window).has_value());

        // Then the finished window onto the screen, once, at half opacity.
        luminaria::GpuTextureFill whole{};
        whole.texture = &offscreen->texture();
        whole.x = 0;
        whole.y = 0;
        whole.w = win_w;
        whole.h = kQuadH;
        whole.alpha = 0.5f;
        const luminaria::GpuTextureFill fills[] = {whole};

        assert(renderer->render_to(*target, black, {}, fills).has_value());
        assert(renderer->read_scanout(*target, pixels).has_value());

        const int only_a = red_at(pixels, kOnlyA, kRow);
        const int overlap = red_at(pixels, kOverlap, kRow);
        const int only_b = red_at(pixels, kOnlyB, kRow);
        assert(near(only_a, 128));
        assert(near(only_b, 128));
        // The point of the whole exercise: no seam. The window is one image, so
        // the overlap is half of white exactly like everywhere else.
        assert(near(overlap, 128));
        assert(overlap < only_a + 8 && overlap > only_a - 8);

        // Outside the window the background shows through undisturbed — the
        // offscreen kept its transparency rather than compositing black into it.
        assert(near(red_at(pixels, kW - 1, kRow), 0));
        assert(near(red_at(pixels, kOnlyA, kH - 1), 0));

        // Rendering into it again must work: the target comes back from being
        // sampled, not from a display engine, and a second pass has to make the
        // transition from that layout. A repeat of the same content, so the
        // screen must not change.
        assert(renderer->render_offscreen(*offscreen, transparent, {}, window).has_value());
        assert(renderer->render_to(*target, black, {}, fills).has_value());
        assert(renderer->read_scanout(*target, pixels).has_value());
        assert(near(red_at(pixels, kOverlap, kRow), 128));
    }

    return 0;
}
