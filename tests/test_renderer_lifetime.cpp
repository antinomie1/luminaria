// The renderer may die first. Everything declared in main() used to have to sit
// after the renderer it cached textures from, or shutdown aborted inside
// vk::raii — and nothing but a crash at exit said so. The order is free now:
//
//   - a texture, a target or a blur chain leases the device, so it can be
//     destroyed after the VulkanRenderer that made it, whether or not it was
//     ever drawn; the last one out destroys the device (run under the validation
//     layer, which counts anything left behind);
//   - a `Frame` that outlives its renderer goes inert and says so;
//   - `LinuxDmabuf` and `ScreencopyManager` stop calling a renderer that is gone;
//   - a dying `SceneRenderer` unbinds its outputs and drops their GPU objects,
//     and `Cursor` and `ScreenManager` keep running on signals without it.
//
// Run it under ASan for the part that matters: a holder that does not look
// before it calls is a heap-use-after-free below. Skips (77) without a Vulkan
// device; the SceneRenderer half has to hold on the CPU path too, and does run.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include <wayland-client.h>
#include <wayland-server-core.h>

#include "xdg-shell-client-protocol.h"

import luminaria.shell;
import std;

#include "shell_harness.h"

namespace {

namespace lm = luminaria;
using harness::Client;
using harness::ManualBackend;
using harness::ManualOutput;
using harness::settle;

constexpr int kW = 16, kH = 16;

/// Declared in the order nothing used to survive: every holder before the
/// renderer it is about to be filled from, so the renderer dies first.
bool textures_outlive_the_renderer() {
    std::optional<lm::GpuTexture> drawn;
    std::optional<lm::GpuTexture> never_drawn;
    std::optional<lm::OffscreenTarget> offscreen;
    std::optional<lm::BlurChain> chain;
    std::optional<lm::ScanoutTarget> scanout;
    std::vector<std::uint8_t> rgba(4 * kW * kH, 0x80);
    std::weak_ptr<const void> alive;
    {
        auto created = lm::VulkanRenderer::create();
        if (!created) {
            std::println(std::cerr, "skip: no Vulkan 1.3 device ({})", created.error().message);
            return false;
        }
        lm::VulkanRenderer renderer = std::move(*created);
        alive = renderer.alive();
        assert(!alive.expired());
        drawn.emplace(*renderer.upload_texture(kW, kH, rgba));
        never_drawn.emplace(*renderer.upload_texture(kW, kH, rgba));
        offscreen.emplace(*renderer.create_offscreen(kW, kH));
        chain.emplace(*renderer.create_blur_chain(kW, kH));
        // Drawn, so that it holds a descriptor set the pool owns, and the frame
        // is still on the GPU when the renderer goes: an out-fence means no wait.
        if (renderer.dmabuf_supported()) {
            const auto mods = renderer.scanout_modifiers(DRM_FORMAT_XRGB8888);
            if (auto target = renderer.create_scanout(kW, kH, DRM_FORMAT_XRGB8888, mods); target) {
                scanout.emplace(std::move(*target));
                int out_fence = -1;
                const lm::RenderSync sync{{}, &out_fence};
                const lm::GpuTextureFill fill{&*drawn, 0, 0, kW, kH};
                assert(renderer.render_to(*scanout, lm::Color{0, 0, 0, 1}, {}, {&fill, 1}, {}, {},
                                          sync));
                if (out_fence >= 0) {
                    close(out_fence);
                }
            }
        }
    }
    assert(alive.expired());
    // In no particular order, and the device goes with the last of them.
    never_drawn.reset();
    chain.reset();
    drawn.reset();
    scanout.reset();
    offscreen.reset();
    assert(lm::vulkan_validation_errors() == 0);
    return true;
}

void frame_outlives_the_renderer() {
    ManualOutput output(kW, kH);
    std::optional<lm::Frame> frame;
    {
        lm::VulkanRenderer renderer = std::move(*lm::VulkanRenderer::create());
        frame.emplace(output, renderer);
        assert(frame->reset(DRM_FORMAT_XRGB8888));
        frame->begin({0, 0, kW, kH});
        frame->place_rect(0, 0, 4, 4, {1, 0, 0, 1});
        assert(frame->submit({0, 0, 0, 1}));
    }
    assert(!frame->reset(DRM_FORMAT_XRGB8888));
    frame->begin({0, 0, kW, kH});
    frame->place_rect(0, 0, 4, 4, {1, 0, 0, 1});
    assert(!frame->submit({0, 0, 0, 1}));
    assert(!frame->read_back());
    frame->send_frame_done(0);
    assert(lm::vulkan_validation_errors() == 0);
}

void protocols_outlive_the_renderer() {
    auto display = lm::must(lm::Display::create(), "display");
    std::optional<lm::LinuxDmabuf> dmabuf;
    std::optional<lm::ScreencopyManager> screencopy;
    {
        lm::VulkanRenderer renderer = std::move(*lm::VulkanRenderer::create());
        dmabuf = lm::to_optional(lm::LinuxDmabuf::create(display, &renderer));
        screencopy = lm::to_optional(lm::ScreencopyManager::create(display));
        if (screencopy.has_value()) {
            screencopy->set_renderer(&renderer);
        }
    }
    // Both are still globals a client can bind, and neither touches the renderer.
    wl_event_loop_dispatch(wl_display_get_event_loop(display.c_ptr()), 0);
}

/// `SceneRenderer` goes first, with everything that holds it still connected to
/// signals that keep firing. Holds on either path.
void scene_renderer_dies_first() {
    auto display = lm::must(lm::Display::create(), "display");
    auto globals = lm::must(lm::CoreGlobals::create(display), "globals");
    lm::Seat& seat = globals.seat();
    lm::OutputLayout layout;
    lm::PopupTree popups(globals.shell());
    ManualBackend backend;
    ManualOutput& output = backend.add(kW, kH);
    ManualOutput& unplugged = backend.add(kW, kH);

    // Every holder before the renderer, which is exactly the order that aborted.
    std::vector<lm::SceneItem> scene{{.kind = lm::SceneItem::Kind::rect, .box = {0, 0, 4, 4}}};
    lm::SceneOutput state;
    std::optional<lm::Cursor> cursor;
    std::optional<lm::ScreenManager> screens;
    std::optional<lm::LinuxDmabuf> dmabuf;
    std::optional<lm::DefaultInputRouter> input;
    auto renderer = std::make_optional<lm::SceneRenderer>();
    renderer->set_presentation(&globals.presentation());
    dmabuf = renderer->linux_dmabuf(display);
    cursor.emplace(*renderer, layout, seat, &globals.cursor_shape());
    screens.emplace(display, backend, *renderer, layout);
    screens->follow_popups(popups);
    screens->set_scene_builder([&](lm::Output&) { return std::span<const lm::SceneItem>{scene}; });
    input.emplace(seat, *cursor, *screens, globals.shell(), popups);
    assert(backend.start());
    const std::weak_ptr<const void> alive = renderer->alive();
    assert(!alive.expired());
    {
        ManualOutput lone(kW, kH);
        assert(renderer->reset(state, lone));
    }
    assert(renderer->reset(state, output)); // a state the renderer has to let go of

    Client client(display);
    settle(client, output);
    const int flips = output.flips;
    input->route(lm::PointerMotionAbsEvent{.x = 2.0, .y = 2.0, .output = &output});
    client.pump();

    renderer.reset();
    assert(alive.expired());
    assert(!state.gpu_output());

    // Everything keeps arriving, and all of it is now a no-op.
    output.schedule_frame();
    output.fire();
    assert(output.flips == flips);
    input->route(lm::PointerMotionEvent{.dx = 1.0, .dy = 1.0});
    cursor->refresh();
    screens->invalidate();
    backend.remove(unplugged); // through the layout, the cursor and the manager
    assert(screens->size() == 1);
    backend.add(kW, kH); // arrives to a manager with nothing to draw on
    assert(screens->size() == 1);
    client.pump();

    // And they all go, in the order they were declared in, which is the reverse
    // of what used to work.
    input.reset();
    dmabuf.reset();
    screens.reset();
    cursor.reset();
}

} // namespace

int main() {
    setenv("LUMINARIA_VK_VALIDATION", "1", 1);
    setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
    const bool gpu = textures_outlive_the_renderer();
    if (gpu) {
        frame_outlives_the_renderer();
        protocols_outlive_the_renderer();
        scene_renderer_dies_first();
    }
    setenv("VK_ICD_FILENAMES", "/nonexistent/luminaria-test-icd.json", 1);
    setenv("VK_DRIVER_FILES", "/nonexistent/luminaria-test-icd.json", 1);
    scene_renderer_dies_first();
    return gpu ? 0 : 77;
}
