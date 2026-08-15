// What a steady-state frame costs the heap, on the renderer's side.
//
// `test_frame` pins this for the shell layer — refilling the placement list
// allocates nothing. The renderer used to be the other half of the story: every
// `render_to()` built a `Region` or three, a `vk::raii::Framebuffer`, a command
// buffer and a fence, freed them all, and did it again 1/60s later. Now the
// regions are the renderer's scratch, the framebuffers live on the target, and
// command buffers and fences come off free lists gated on their submit's fence.
//
// So: warm everything up, then count C++ heap allocations across a run of
// identical frames. The count must be zero. (This cannot see what the Vulkan
// driver mallocs internally — operator new is not malloc — which is precisely
// why it is worth pinning the part we do control.)
//
// Skips (77) without a Vulkan device.
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <drm_fourcc.h>

import luminaria.gpu;

namespace {
std::atomic<std::size_t> g_allocs{0};
constexpr int kW = 128, kH = 96;
} // namespace

void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

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

    // Two overlapping windows, the front one opaque: enough to exercise the
    // occlusion arithmetic (subtract, intersect, coalesce) rather than the
    // trivial single-region path.
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(32) * 32 * 4, 0xC0);
    auto back = renderer->upload_texture(32, 32, pixels);
    auto front = renderer->upload_texture(32, 32, pixels);
    if (!back || !front) {
        std::fprintf(stderr, "skip: could not upload textures\n");
        return 77;
    }

    const luminaria::Box front_box{20, 20, 32, 32};
    const luminaria::Box opaque[1] = {luminaria::Box{20, 20, 32, 32}};
    std::vector<luminaria::GpuTextureFill> fills(2);
    fills[0].texture = &*back;
    fills[0].x = 8;
    fills[0].y = 8;
    fills[0].w = 32;
    fills[0].h = 32;
    fills[1].texture = &*front;
    fills[1].x = front_box.x;
    fills[1].y = front_box.y;
    fills[1].w = front_box.width;
    fills[1].h = front_box.height;
    fills[1].opaque = std::span<const luminaria::Box>{opaque};

    // Two disjoint damage boxes, so the repaint really is a region and not one
    // rectangle: this is what a window and a cursor look like.
    const luminaria::Box damage[2] = {luminaria::Box{8, 8, 40, 40}, luminaria::Box{90, 70, 16, 16}};
    const luminaria::OutputMapping mapping{luminaria::Transform::normal, 1};
    const luminaria::Color bg{0.1f, 0.1f, 0.13f, 1.0f};

    // Warm-up: the first frame is a full clear, the second onwards are partial
    // repaints, and the free lists fill from the frames that have retired.
    for (int i = 0; i < 8; ++i) {
        auto s = renderer->render_to(*target, bg, {}, fills, damage, mapping);
        assert(s.has_value());
    }

    const std::size_t before = g_allocs.load(std::memory_order_relaxed);
    for (int i = 0; i < 32; ++i) {
        auto s = renderer->render_to(*target, bg, {}, fills, damage, mapping);
        assert(s.has_value());
    }
    const std::size_t spent = g_allocs.load(std::memory_order_relaxed) - before;
    if (spent != 0) {
        std::fprintf(stderr, "render_to allocated %zu times across 32 steady-state frames\n",
                     spent);
    }
    assert(spent == 0);

    // The other path: an out-fence, so the render is not waited for and its
    // command buffer, fence and semaphores live in the in-flight list until
    // the GPU retires them. That is how a real display runs — the free lists
    // have to come back round there too.
    int out_fence = -1;
    const luminaria::RenderSync sync{{}, &out_fence};
    // Stand in for the display: wait for the render's own sync_file before
    // asking for the next frame, the way a page flip paces a real compositor.
    // Without that this loop submits as fast as the CPU can and the in-flight
    // list grows to a depth no display would ever produce.
    auto flip = [&] {
        if (out_fence < 0) {
            return;
        }
        pollfd pfd{out_fence, POLLIN, 0};
        (void)poll(&pfd, 1, 1000);
        close(out_fence);
        out_fence = -1;
    };
    for (int i = 0; i < 8; ++i) {
        auto s = renderer->render_to(*target, bg, {}, fills, damage, mapping, sync);
        assert(s.has_value());
        flip();
    }
    const std::size_t before_async = g_allocs.load(std::memory_order_relaxed);
    int fences = 0;
    for (int i = 0; i < 32; ++i) {
        auto s = renderer->render_to(*target, bg, {}, fills, damage, mapping, sync);
        assert(s.has_value());
        fences += out_fence >= 0 ? 1 : 0;
        flip();
    }
    const std::size_t async_spent = g_allocs.load(std::memory_order_relaxed) - before_async;
    std::fprintf(stderr,
                 "steady state: %zu allocations composited, %zu across 32 frames handing back "
                 "%d out-fences\n",
                 spent, async_spent, fences);
    // The imported semaphores are per-frame by nature, but the list holding them
    // is recycled with the submit slot, so this path allocates nothing either.
    // (A device without VK_KHR_external_semaphore_fd hands back no fences and
    // takes the waited path above, which is fine — it is the same assertion.)
    assert(async_spent == 0);
    return 0;
}
