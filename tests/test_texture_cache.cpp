// Descriptor sets are cached per texture and recycled when the texture dies,
// rather than a fresh pool being built every frame. This checks that the
// recycling never shows the wrong picture: churn 200 textures of distinct
// colours through create-draw-destroy — well past the 64 a pool holds, so the
// free list and not just fresh allocation is doing the work — and verify every
// frame's pixels; then draw two live textures at once, the same texture twice
// in one frame, and a texture that has been sitting in the cache while others
// came and went.
//
// What it does NOT prove is the in-flight rule (a retired set must not be
// rewritten while a submitted frame may still be sampling through it). That
// corrupts a frame already on its way to the display, and every way of reading
// a frame back forces a GPU sync first, so it is invisible from out here —
// running under VK_LAYER_KHRONOS_validation is what catches it.
//
// Skips (77) without a Vulkan device or dmabuf support.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <unistd.h>

#include <drm_fourcc.h>

import luminaria.gpu;

namespace {

constexpr int kW = 8, kH = 8;

std::vector<std::uint8_t> solid(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    std::vector<std::uint8_t> px(2 * 2 * 4);
    for (std::size_t i = 0; i < 4; ++i) {
        px[i * 4 + 0] = r;
        px[i * 4 + 1] = g;
        px[i * 4 + 2] = b;
        px[i * 4 + 3] = 255;
    }
    return px;
}

} // namespace

int main() {
    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77;
    }
    if (!renderer->dmabuf_supported()) {
        std::fprintf(stderr, "skip: GPU has no dmabuf import/export\n");
        return 77;
    }

    const auto mods = renderer->scanout_modifiers(DRM_FORMAT_XRGB8888);
    auto target = renderer->create_scanout(kW, kH, DRM_FORMAT_XRGB8888, mods);
    if (!target) {
        std::fprintf(stderr, "skip: %s\n", target.error().message.c_str());
        return 77;
    }
    const luminaria::DmabufPlane& p = target->plane();

    auto read = [&](std::vector<std::uint8_t>& out) {
        auto pixels = renderer->import_dmabuf(p.fd, p.width, p.height, p.format, p.offset, p.stride,
                                              p.modifier);
        assert(pixels.has_value());
        out = std::move(*pixels);
    };
    auto at = [&](const std::vector<std::uint8_t>& px, int x, int y) {
        const std::size_t i = (static_cast<std::size_t>(y) * kW + x) * 4;
        return std::vector<std::uint8_t>{px[i], px[i + 1], px[i + 2]};
    };

    // --- churn: one texture per round, destroyed before the next ---
    //
    // Far more rounds than the 64 sets a pool holds, so the free list is what
    // is being exercised and not just fresh allocation.
    //
    // Half the rounds ask for an out-fence, so the asynchronous submit path —
    // where a frame is still running when its texture is destroyed and the set
    // retired — is walked as often as the blocking one.
    constexpr int kRounds = 200;
    std::vector<std::uint8_t> pixels;
    for (int i = 0; i < kRounds; ++i) {
        // A colour that changes every round: a stale set would show a previous
        // one rather than merely "some red".
        const auto r = static_cast<std::uint8_t>(20 + (i * 7) % 200);
        const auto g = static_cast<std::uint8_t>(20 + (i * 13) % 200);
        const auto b = static_cast<std::uint8_t>(20 + (i * 29) % 200);
        auto texture = renderer->upload_texture(2, 2, solid(r, g, b));
        assert(texture.has_value());
        const luminaria::GpuTextureFill fill{&*texture, 0, 0, kW, kH};
        int out_fence = -1;
        const bool async = (i % 2) == 0;
        const luminaria::RenderSync sync{{}, async ? &out_fence : nullptr};
        assert(renderer->render_to(*target, luminaria::Color{0, 0, 0, 1}, {}, {&fill, 1}, {}, {},
                                   sync));
        // import_dmabuf goes through its own submit-and-wait, so the frame is
        // on the surface by the time the pixels come back either way.
        read(pixels);
        assert((at(pixels, 4, 4) == std::vector<std::uint8_t>{r, g, b}));
        if (out_fence >= 0) {
            close(out_fence);
        }
    }

    // --- two live at once: one set each, whatever the recycling did ---
    auto left = renderer->upload_texture(2, 2, solid(255, 0, 0));
    auto right = renderer->upload_texture(2, 2, solid(0, 255, 0));
    assert(left.has_value() && right.has_value());
    const luminaria::GpuTextureFill both[2] = {
        luminaria::GpuTextureFill{&*left, 0, 0, kW / 2, kH},
        luminaria::GpuTextureFill{&*right, kW / 2, 0, kW / 2, kH},
    };
    assert(renderer->render_to(*target, luminaria::Color{0, 0, 255, 1}, {}, both));
    read(pixels);
    assert((at(pixels, 1, 4) == std::vector<std::uint8_t>{255, 0, 0}));
    assert((at(pixels, 6, 4) == std::vector<std::uint8_t>{0, 255, 0}));

    // Drawing the same texture twice in one frame shares the one cached set:
    // the second quad must still sample it, not whatever was bound before.
    const luminaria::GpuTextureFill twice[2] = {
        luminaria::GpuTextureFill{&*left, 0, 0, kW / 2, kH},
        luminaria::GpuTextureFill{&*left, kW / 2, 0, kW / 2, kH},
    };
    assert(renderer->render_to(*target, luminaria::Color{0, 0, 255, 1}, {}, twice));
    read(pixels);
    assert((at(pixels, 1, 4) == std::vector<std::uint8_t>{255, 0, 0}));
    assert((at(pixels, 6, 4) == std::vector<std::uint8_t>{255, 0, 0}));

    // And the cache survives its texture being re-drawn after other textures
    // have come and gone in between.
    for (int i = 0; i < 10; ++i) {
        auto scratch = renderer->upload_texture(2, 2, solid(1, 2, 3));
        assert(scratch.has_value());
        const luminaria::GpuTextureFill f{&*scratch, 0, 0, kW, kH};
        assert(renderer->render_to(*target, luminaria::Color{0, 0, 0, 1}, {}, {&f, 1}));
    }
    const luminaria::GpuTextureFill again{&*right, 0, 0, kW, kH};
    assert(renderer->render_to(*target, luminaria::Color{0, 0, 0, 1}, {}, {&again, 1}));
    read(pixels);
    assert((at(pixels, 4, 4) == std::vector<std::uint8_t>{0, 255, 0}));
    return 0;
}
