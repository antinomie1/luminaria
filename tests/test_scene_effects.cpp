// The effects, as a compositor actually reaches them: fields on a `SceneItem`.
//
// Everything below this file is already covered — the SDFs in
// test_rounded_shadow, the chain in test_blur_chain, the backdrop in
// test_backdrop_blur. What is only reachable here is the wiring: that
// `alpha`, `corner_radius`, `blur` and `shadow` on a scene item survive
// `SceneRenderer::present()` and land on the screen, and that an item which
// asks for none of them still costs nothing.
//
// The scene is deliberately client-free. A surface item needs a live client to
// say anything, and the compositors that drive one cover it; rect and image
// items go down the same effect paths and are assertable from here.
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

const luminaria::Pixel& at(const std::vector<luminaria::Pixel>& px, int x, int y) {
    return px[static_cast<std::size_t>(y) * kW + x];
}

} // namespace

int main() {
    luminaria::SceneRenderer scene_renderer;
    if (!scene_renderer.gpu_enabled()) {
        std::fprintf(stderr, "skip: %s\n", std::string(scene_renderer.gpu_error()).c_str());
        return 77;
    }
    auto display = luminaria::Display::create();
    assert(display.has_value());
    luminaria::HeadlessOutput output(display->event_loop(), kW, kH, 1);
    luminaria::SceneOutput state;
    if (auto ready = scene_renderer.reset(state, output, kXrgb8888); !ready) {
        std::fprintf(stderr, "skip: %s\n", ready.error().message.c_str());
        return 77;
    }
    if (!state.gpu_output()) {
        std::fprintf(stderr, "skip: no GPU output for this headless target\n");
        return 77;
    }

    const luminaria::Color black{0, 0, 0, 1};
    const auto show = [&](std::span<const luminaria::SceneItem> scene) {
        return scene_renderer.present(state, output, scene, black, {});
    };

    // --- opacity -------------------------------------------------------------
    // A solid item at half alpha over a black background is half its colour.
    // Nothing composes offscreen for a rectangle, so this is the cheap arm of
    // `alpha` and the one that has to keep working when the expensive one
    // cannot allocate.
    {
        const luminaria::SceneItem items[] = {
            {.kind = luminaria::SceneItem::Kind::rect,
             .box = {0, 0, kW, kH},
             .color = {1, 1, 1, 1},
             .alpha = 0.5f},
        };
        assert(show(items).has_value());
        const luminaria::Pixel half = at(output.last_frame(), kW / 2, kH / 2);
        assert(half.r > 100 && half.r < 155);
    }

    // --- corner radius -------------------------------------------------------
    // Same rectangle, opaque, with a radius big enough that the corner texel is
    // outside the rounded box: it stays background while the middle does not.
    {
        const luminaria::SceneItem items[] = {
            {.kind = luminaria::SceneItem::Kind::rect,
             .box = {8, 8, kW - 16, kH - 16},
             .color = {1, 1, 1, 1}},
        };
        assert(show(items).has_value());
        assert(at(output.last_frame(), 9, 9).r > 200); // square, for the contrast
    }
    {
        // An image item, because a radius applies to a single texture and a
        // solid rectangle is not one — this is the placement the bar uses.
        const std::vector<luminaria::Pixel> white(static_cast<std::size_t>(48) * 48,
                                                  luminaria::Pixel{255, 255, 255, 255});
        const luminaria::SceneItem items[] = {
            {.kind = luminaria::SceneItem::Kind::image,
             .box = {8, 8, 48, 48},
             .pixels = white,
             .accepts_input = false,
             .corner_radius = 12.0f},
        };
        assert(show(items).has_value());
        const std::vector<luminaria::Pixel>& px = output.last_frame();
        assert(at(px, 9, 9).r < 60);           // bitten off by the radius
        assert(at(px, 32, 32).r > 200);        // and untouched in the middle
        assert(at(px, 32, 9).r > 200);         // ... and along the top edge
    }

    // --- shadow --------------------------------------------------------------
    // A feathered shadow under a small opaque rectangle reaches outside the
    // caster and fades. The caster is white so "the shadow is there" cannot be
    // confused with "the item is there".
    {
        const luminaria::SceneItem items[] = {
            {.kind = luminaria::SceneItem::Kind::rect,
             .box = {24, 24, 16, 16},
             .color = {1, 1, 1, 1},
             .shadow = {.color = {1, 0, 0, 1}, .feather = 10.0f}},
        };
        assert(show(items).has_value());
        const std::vector<luminaria::Pixel>& px = output.last_frame();
        assert(at(px, 32, 32).r > 200 && at(px, 32, 32).g > 200); // the caster
        assert(at(px, 32, 20).r > 40 && at(px, 32, 20).g < 40);   // red, just outside
        assert(at(px, 32, 2).r < 20);                             // gone by the edge
    }

    // --- blur, two windows --------------------------------------------------
    // Two real (non-x-ray) blurred items in one scene: each gets its own
    // backdrop+chain cache, and the second allocation used to move the first
    // cache out from under its queued capture — the resize-on-demand growth
    // reallocated the cache vector between the two queue_blur calls, leaving
    // the first job's OffscreenTarget/BlurChain pointers dangling into freed
    // storage by the time submit() ran them. This is the first frame that
    // blurs at all, so the vector really does grow here. Both regions must
    // actually mix, not merely fail to crash.
    {
        const luminaria::SceneItem items[] = {
            {.kind = luminaria::SceneItem::Kind::rect, .box = {0, 0, 32, kH}, .color = {1, 0, 0, 1}},
            {.kind = luminaria::SceneItem::Kind::rect,
             .box = {32, 0, kW - 32, kH},
             .color = {0, 0, 1, 1}},
            {.kind = luminaria::SceneItem::Kind::rect,
             .box = {16, 4, 32, 24},
             .color = {0, 0, 0, 0}, // draws nothing; the blur is the visible part
             .blur = {.enabled = true, .params = {.passes = 2, .offset = 2.0f}}},
            {.kind = luminaria::SceneItem::Kind::rect,
             .box = {16, 36, 32, 24},
             .color = {0, 0, 0, 0},
             .blur = {.enabled = true, .params = {.passes = 2, .offset = 2.0f}}},
        };
        assert(show(items).has_value());
        const std::vector<luminaria::Pixel>& px = output.last_frame();
        assert(at(px, 4, 32).r > 200 && at(px, 4, 32).b < 40); // outside both, sharp
        const luminaria::Pixel first = at(px, 35, 10);         // inside the first region
        const luminaria::Pixel second = at(px, 35, 44);        // inside the second
        assert(first.r > 30 && first.b > 30);
        assert(second.r > 30 && second.b > 30);
    }

    // --- blur ----------------------------------------------------------------
    // Two hard-edged halves under a blurred item: inside it the two have mixed,
    // which is the whole point of the backdrop path.
    {
        const luminaria::SceneItem items[] = {
            {.kind = luminaria::SceneItem::Kind::rect, .box = {0, 0, 32, kH}, .color = {1, 0, 0, 1}},
            {.kind = luminaria::SceneItem::Kind::rect,
             .box = {32, 0, kW - 32, kH},
             .color = {0, 0, 1, 1}},
            {.kind = luminaria::SceneItem::Kind::rect,
             .box = {16, 8, 32, 48},
             .color = {0, 0, 0, 0}, // draws nothing; the blur is the visible part
             .blur = {.enabled = true, .params = {.passes = 2, .offset = 2.0f}}},
        };
        assert(show(items).has_value());
        const std::vector<luminaria::Pixel>& px = output.last_frame();
        assert(at(px, 4, 32).r > 200 && at(px, 4, 32).b < 40); // outside, untouched
        const luminaria::Pixel mixed = at(px, 35, 32);
        assert(mixed.r > 30 && mixed.b > 30);
    }

    // --- nothing asked for ---------------------------------------------------
    // The item that wants no effect is the common case, and drawing the same
    // one twice must still answer `unchanged`. An effect path entered by an
    // item that did not ask for it shows up here as a frame that keeps
    // repainting (ADR 0002).
    {
        const luminaria::SceneItem items[] = {
            {.kind = luminaria::SceneItem::Kind::rect, .box = {4, 4, 20, 20}, .color = {0, 1, 0, 1}},
        };
        assert(show(items).has_value());
        const auto again = show(items);
        assert(again.has_value() && *again == luminaria::SceneOutcome::unchanged);
    }

    return 0;
}
