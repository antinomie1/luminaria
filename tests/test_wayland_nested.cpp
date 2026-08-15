#include <typeinfo>
// Nested backend against a real parent compositor: connect, open a window, and
// present frames. Skips (exit 77) when no parent compositor is available.
//
// Also exercises the zero-copy present path: when the parent implements
// zwp_linux_dmabuf_v1 and we have a Vulkan device, a renderer scanout target is
// handed straight to the parent as a wl_buffer with no pixel ever copied. Both
// halves of that are optional at runtime, so the check is conditional rather
// than a skip — the shm path above is tested either way.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <drm_fourcc.h>

import luminaria;

int main() {
    if (std::getenv("WAYLAND_DISPLAY") == nullptr) {
        std::fprintf(stderr, "skip: no WAYLAND_DISPLAY (no parent compositor)\n");
        return 77;
    }

    auto display = luminaria::Display::create();
    assert(display.has_value());

    auto backend = luminaria::WaylandBackend::create(display->event_loop());
    if (!backend) {
        std::fprintf(stderr, "skip: %s\n", backend.error().message.c_str());
        return 77;
    }

    luminaria::Output& output = backend->add_output(400, 300);

    // --- zero-copy present, if this machine can do it at all ---
    //
    // The nested backend must report exactly what the parent advertised: an
    // empty list means "no dmabuf path here", and a caller that gets one is
    // supposed to fall back rather than allocate a target nobody will take.
    const std::vector<std::uint64_t> host = output.scanout_modifiers(DRM_FORMAT_XRGB8888);
    auto r = luminaria::VulkanRenderer::create();
    if (!host.empty() && r) {
        std::vector<std::uint64_t> both;
        for (std::uint64_t m : r->scanout_modifiers(DRM_FORMAT_XRGB8888)) {
            if (std::find(host.begin(), host.end(), m) != host.end()) {
                both.push_back(m);
            }
        }
        if (!both.empty()) {
            auto target = r->create_scanout(400, 300, DRM_FORMAT_XRGB8888, both);
            assert(target.has_value());
            // The modifier actually chosen has to be one we asked for; handing
            // the parent a buffer in a layout it never advertised is exactly
            // the bug this negotiation exists to prevent.
            assert(std::find(both.begin(), both.end(), target->plane().modifier) != both.end());
            auto id = output.import_scanout(target->plane());
            assert(id.has_value());
            // Importing twice yields two distinct ids: a compositor flips
            // between targets and must be able to hold both at once.
            auto again = output.import_scanout(target->plane());
            assert(again.has_value() && *again != *id);
            assert(r->render_to(*target, luminaria::Color{0.1f, 0.2f, 0.3f, 1.0f}, {}, {})
                       .has_value());
            assert(output.commit_scanout(*id).has_value());
            // An id that was never handed out is refused, not scanned out.
            assert(!output.commit_scanout(*again + 1000).has_value());
            std::fprintf(stderr, "nested: zero-copy dmabuf present ok (modifier %#llx)\n",
                         static_cast<unsigned long long>(target->plane().modifier));
        }
    }

    bool saw_output = false;
    int frames = 0;
    luminaria::Signal<luminaria::FrameEvent>::Connection frame_conn;

    auto out_conn = backend->new_output.connect([&](luminaria::NewOutput& e) {
        saw_output = true;
        assert(e.output.width() == 400 && e.output.height() == 300);
        frame_conn = e.output.frame.connect([&](luminaria::FrameEvent& fe) {
            (void)fe.output.commit(luminaria::Color{0.2f, 0.4f, 0.8f, 1.0f});
            if (++frames >= 3) {
                display->terminate();
            }
        });
    });

    assert(backend->start().has_value());

    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(3000);

    display->run();

    assert(saw_output);
    assert(frames >= 1);
    return 0;
}
