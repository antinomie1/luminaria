// luminaria-tty — a real bare-metal compositor. DRM output + libinput input +
// wl_compositor/xdg-shell/seat, compositing client windows on the GPU and
// scanning them out to the monitor.
//
// Run from a free VT with the desktop stopped (see README). It prints its
// WAYLAND_DISPLAY; point a client at it from another VT/ssh, e.g.:
//     WAYLAND_DISPLAY=wayland-1 weston-simple-shm
// Esc quits.
//
// TODO: windows cascade at fixed offsets (no move/resize/stacking UI); a
// software cursor and pointer-focus hit-testing are left out — keyboard focus
// follows the latest mapped window.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <vector>

#include "luminaria/backend/drm.hpp"
#include "luminaria/backend/libinput.hpp"
#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"
#include "luminaria/render/vulkan.hpp"
#include "luminaria/seat.hpp"
#include "luminaria/xdg_shell.hpp"

namespace {
constexpr uint32_t KEY_ESC = 1;

struct Window {
    luminaria::Toplevel* toplevel = nullptr;
    int x = 0, y = 0;
    bool mapped = false;
    luminaria::Signal<luminaria::ToplevelMap>::Connection on_map;
    luminaria::Signal<luminaria::ToplevelDestroy>::Connection on_destroy;
};
} // namespace

int main(int argc, char** argv) {
    const char* env = std::getenv("LUMINARIA_DRM_DEVICE");
    const char* device = argc > 1 ? argv[1] : env;

    auto display = luminaria::Display::create();
    if (!display) {
        return 1;
    }
    if (auto s = display->init_shm(); !s) {
        std::fprintf(stderr, "luminaria-tty: shm: %s\n", s.error().message.c_str());
    }

    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "luminaria-tty: vulkan: %s\n", renderer.error().message.c_str());
        return 1;
    }
    auto drm = device != nullptr ? luminaria::DrmBackend::create(display->event_loop(), device)
                                 : luminaria::DrmBackend::create(display->event_loop());
    if (!drm) {
        std::fprintf(stderr, "luminaria-tty: drm: %s\n", drm.error().message.c_str());
        return 1;
    }
    auto input = luminaria::LibinputBackend::create(display->event_loop());
    if (!input) {
        std::fprintf(stderr, "luminaria-tty: input: %s\n", input.error().message.c_str());
        return 1;
    }

    auto compositor = luminaria::Compositor::create(*display);
    auto shell = luminaria::XdgShell::create(*display);
    auto seat = luminaria::Seat::create(*display);
    if (!compositor || !shell || !seat) {
        std::fprintf(stderr, "luminaria-tty: protocol setup failed\n");
        return 1;
    }

    std::list<Window> windows; // stable addresses; slots kept for the process life

    auto new_toplevel = shell->new_toplevel().connect([&](luminaria::NewToplevel& e) {
        Window& w = windows.emplace_back();
        w.toplevel = &e.toplevel;
        const int n = static_cast<int>(windows.size()) - 1;
        w.x = 40 + 30 * n;
        w.y = 40 + 30 * n;
        w.on_map = e.toplevel.map.connect([&w, &seat](luminaria::ToplevelMap&) {
            w.mapped = true;
            seat->set_keyboard_focus(&w.toplevel->surface());
            seat->pointer_enter(w.toplevel->surface(), 0, 0);
        });
        w.on_destroy = e.toplevel.destroy.connect([&w](luminaria::ToplevelDestroy&) {
            w.mapped = false;
            w.toplevel = nullptr;
        });
    });

    // libinput -> seat.
    auto key_conn = input->key().connect([&](luminaria::KeyEvent& e) {
        if (e.pressed && e.keycode == KEY_ESC) {
            display->terminate();
        } else {
            seat->notify_key(e.keycode, e.pressed);
        }
    });
    auto btn_conn = input->pointer_button().connect(
        [&](luminaria::PointerButtonEvent& e) { seat->pointer_button(e.button, e.pressed); });

    // DRM frame -> composite all mapped client windows -> scan out.
    luminaria::Signal<luminaria::FrameEvent>::Connection frame_conn;
    auto out_conn = drm->new_output.connect([&](luminaria::NewOutput& e) {
        const int ow = e.output.width();
        const int oh = e.output.height();
        std::printf("luminaria-tty: output %dx%d\n", ow, oh);
        frame_conn = e.output.frame.connect([&, ow, oh](luminaria::FrameEvent& fe) {
            std::vector<std::vector<std::uint8_t>> holds;
            std::vector<luminaria::TextureFill> textures;
            for (Window& w : windows) {
                if (!w.mapped || w.toplevel == nullptr) {
                    continue;
                }
                std::vector<std::uint8_t> rgba;
                int bw = 0, bh = 0;
                if (w.toplevel->surface().current_buffer_rgba(rgba, bw, bh) && bw > 0 && bh > 0) {
                    holds.push_back(std::move(rgba));
                    textures.push_back({w.x, w.y, bw, bh, holds.back().data()});
                }
            }
            auto px = renderer->composite(ow, oh, luminaria::Color{0.1f, 0.1f, 0.13f, 1.0f}, {}, textures);
            if (px) {
                (void)fe.output.commit_frame(*px, ow, oh);
            } else {
                (void)fe.output.commit(luminaria::Color{0.1f, 0.1f, 0.13f, 1.0f});
            }
        });
    });

    if (auto socket = display->add_socket_auto()) {
        setenv("WAYLAND_DISPLAY", socket->c_str(), 1);
        std::printf("luminaria-tty: WAYLAND_DISPLAY=%s — run a client against it. Esc quits.\n",
                    socket->c_str());
    }

    if (auto s = input->start(); !s) {
        std::fprintf(stderr, "luminaria-tty: input start: %s\n", s.error().message.c_str());
    }
    (void)drm->start();
    display->run();
    return 0;
}
