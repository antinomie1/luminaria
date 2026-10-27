// A texture or a render target may die while the frame that used it is still
// on the GPU: a client closes its window, a monitor is unplugged, the
// compositor exits. Their Vulkan images must then outlive them until the
// timeline says that frame is done — destroying the view or freeing the memory
// under a pending command buffer is undefined behaviour, and on real drivers a
// fault or garbage on screen. Descriptor sets were already retired that way;
// the images were not, and only the validation layer could tell.
//
// So this runs under the layer (it sets LUMINARIA_VK_VALIDATION itself) and
// asserts its error count stays 0 while it destroys every kind of object
// straight after an asynchronous submit, with no readback in between to force
// a sync. Without the layer installed it still runs, but proves nothing beyond
// "does not crash", and says so.
//
// Skips (77) without a Vulkan device or dmabuf support.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <unistd.h>

#include <drm_fourcc.h>

import luminaria.render;
import std;

namespace {

constexpr int kW = 16, kH = 16;

std::vector<std::uint8_t> solid(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    std::vector<std::uint8_t> px(4 * 4 * 4);
    for (std::size_t i = 0; i < px.size() / 4; ++i) {
        px[i * 4 + 0] = r;
        px[i * 4 + 1] = g;
        px[i * 4 + 2] = b;
        px[i * 4 + 3] = 255;
    }
    return px;
}

/// One frame that is still running when this returns: an out-fence makes
/// render_to submit without waiting.
void render_async(luminaria::VulkanRenderer& renderer, luminaria::ScanoutTarget& target,
                  const luminaria::GpuTexture* texture) {
    int out_fence = -1;
    const luminaria::RenderSync sync{{}, &out_fence};
    const luminaria::GpuTextureFill fill{texture, 0, 0, kW, kH};
    const std::span<const luminaria::GpuTextureFill> fills =
        texture != nullptr ? std::span{&fill, 1} : std::span<const luminaria::GpuTextureFill>{};
    assert(renderer.render_to(target, luminaria::Color{0, 0, 0, 1}, {}, fills, {}, {}, sync));
    if (out_fence >= 0) {
        close(out_fence);
    }
}

} // namespace

int main() {
    setenv("LUMINARIA_VK_VALIDATION", "1", 1);
    setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);

    {
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

        // A client's texture, destroyed right after the frame that samples it
        // was submitted — a window closing.
        for (int i = 0; i < 20; ++i) {
            auto texture = renderer->upload_texture(4, 4, solid(static_cast<std::uint8_t>(i * 10),
                                                                 80, 160));
            assert(texture.has_value());
            render_async(*renderer, *target, &*texture);
        } // each texture dies here, its frame possibly still running

        // A render target, destroyed right after a frame was drawn into it —
        // a monitor unplugged mid-frame.
        for (int i = 0; i < 5; ++i) {
            auto doomed = renderer->create_scanout(kW, kH, DRM_FORMAT_XRGB8888, mods);
            assert(doomed.has_value());
            auto texture = renderer->upload_texture(4, 4, solid(200, 30, 30));
            assert(texture.has_value());
            render_async(*renderer, *doomed, &*texture);
        }

        // More frames, so the retired images are reaped while the renderer
        // lives — the other half of the rule.
        for (int i = 0; i < 5; ++i) {
            render_async(*renderer, *target, nullptr);
        }

        // And the last of each still pending when everything is torn down: the
        // compositor exiting.
        auto last = renderer->upload_texture(4, 4, solid(1, 2, 3));
        assert(last.has_value());
        render_async(*renderer, *target, &*last);
    }

    const std::uint64_t errors = luminaria::vulkan_validation_errors();
    std::fprintf(stderr, "validation errors: %llu%s\n", static_cast<unsigned long long>(errors),
                 " (0 proves nothing if the layer is not installed; see the log above)");
    assert(errors == 0);
    return errors == 0 ? 0 : 1;
}
