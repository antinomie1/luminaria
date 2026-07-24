// tinyluminaria — a minimal reference compositor built on luminaria. Mirrors tinywl: wire
// up the backend, compositor, xdg-shell, seat, and scene, then run the loop.
//
// Runs on the headless backend (no GPU/display needed to smoke-test the wiring).
// Set LUMINARIA_EXIT_MS to auto-terminate after N ms (used by the smoke test).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "luminaria/backend/headless.hpp"
#include "luminaria/backend/wayland.hpp"
#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"
#include "luminaria/output_global.hpp"
#include "luminaria/render/vulkan.hpp"
#include "luminaria/seat.hpp"
#include "luminaria/xdg_shell.hpp"

namespace {
template <class T>
T must(luminaria::Result<T> r, const char* what) {
    if (!r) {
        std::fprintf(stderr, "tinyluminaria: %s: %s\n", what, r.error().message.c_str());
        std::exit(1);
    }
    return std::move(*r);
}

struct Window {
    luminaria::Toplevel* toplevel = nullptr;
    int x = 0, y = 0;
    int bw = 0, bh = 0; // last composited buffer size, for pointer hit-testing
    bool mapped = false;
    luminaria::Signal<luminaria::ToplevelMap>::Connection on_map;
    luminaria::Signal<luminaria::ToplevelDestroy>::Connection on_destroy;
};
} // namespace

int main() {
    auto display = must(luminaria::Display::create(), "display");
    if (auto s = display.init_shm(); !s) {
        std::fprintf(stderr, "tinyluminaria: shm: %s\n", s.error().message.c_str());
    }

    // Prefer a nested window inside a parent compositor; fall back to headless.
    // LUMINARIA_BACKEND=headless forces headless.
    const char* want = std::getenv("LUMINARIA_BACKEND");
    const bool force_headless = want != nullptr && std::string(want) == "headless";

    std::unique_ptr<luminaria::HeadlessBackend> headless;
    std::unique_ptr<luminaria::WaylandBackend> nested;
    luminaria::Backend* backend = nullptr;
    if (!force_headless) {
        if (auto wb = luminaria::WaylandBackend::create(display.event_loop())) {
            nested = std::make_unique<luminaria::WaylandBackend>(std::move(*wb));
            nested->add_output(800, 600);
            backend = nested.get();
            std::printf("tinyluminaria: nested backend (window in parent compositor)\n");
        }
    }
    if (backend == nullptr) {
        headless = std::make_unique<luminaria::HeadlessBackend>(display.event_loop());
        headless->add_output(800, 600);
        backend = headless.get();
        std::printf("tinyluminaria: headless backend\n");
    }

    auto compositor = must(luminaria::Compositor::create(display), "compositor");
    auto shell = must(luminaria::XdgShell::create(display), "xdg-shell");
    auto seat = must(luminaria::Seat::create(display), "seat");
    // Real clients (weston-terminal) won't map a window until they see an output.
    auto output_global = must(luminaria::OutputGlobal::create(display, 800, 600), "wl_output");

    // Optional GPU compositor. Without it (no Vulkan device) we fall back to a
    // solid background — the window still runs, just isn't drawn.
    std::unique_ptr<luminaria::VulkanRenderer> renderer;
    if (auto r = luminaria::VulkanRenderer::create()) {
        renderer = std::make_unique<luminaria::VulkanRenderer>(std::move(*r));
        std::printf("tinyluminaria: compositing = GPU (Vulkan)\n");
    } else {
        std::printf("tinyluminaria: compositing = DISABLED, background only (no vulkan: %s)\n",
                    r.error().message.c_str());
    }

    std::list<Window> windows; // stable addresses for the process lifetime
    Window* ptr_focus = nullptr; // window under the cursor (nested input routing)
    std::vector<luminaria::Signal<luminaria::FrameEvent>::Connection> frame_conns;
    const luminaria::Color kBg{0.1f, 0.1f, 0.12f, 1.0f};

    auto new_output = backend->new_output.connect([&](luminaria::NewOutput& e) {
        const int ow = e.output.width();
        const int oh = e.output.height();
        frame_conns.push_back(e.output.frame.connect([&, ow, oh](luminaria::FrameEvent& fe) {
            // Reap closed windows here (safe point) — not in the destroy callback,
            // which would free the Window while its own slot is running. Clear the
            // cursor focus if it pointed at a reaped window (avoids a dangling ptr).
            std::erase_if(windows, [&](const Window& w) {
                const bool dead = w.toplevel == nullptr;
                if (dead && ptr_focus == &w) {
                    ptr_focus = nullptr;
                }
                return dead;
            });
            // Composite mapped client windows onto the background, present the frame.
            // Falls back to a solid fill if there's no renderer or nothing to show.
            std::vector<std::vector<std::uint8_t>> holds;
            std::vector<luminaria::TextureFill> textures;
            if (renderer) {
                for (Window& w : windows) {
                    if (!w.mapped || w.toplevel == nullptr) {
                        continue;
                    }
                    std::vector<std::uint8_t> rgba;
                    int bw = 0, bh = 0;
                    if (w.toplevel->surface().current_buffer_rgba(rgba, bw, bh) && bw > 0 &&
                        bh > 0) {
                        w.bw = bw;
                        w.bh = bh;
                        holds.push_back(std::move(rgba));
                        textures.push_back({w.x, w.y, bw, bh, holds.back().data()});
                    }
                }
                static int frame_n = 0;
                if (++frame_n % 60 == 1) {
                    std::printf("tinyluminaria: frame %d — %zu window(s), %zu texture(s)\n",
                                frame_n, windows.size(), textures.size());
                }
                if (auto px = renderer->composite(ow, oh, kBg, {}, textures)) {
                    if (auto s = fe.output.commit_frame(*px, ow, oh); !s) {
                        std::fprintf(stderr, "tinyluminaria: commit_frame: %s\n",
                                     s.error().message.c_str());
                    }
                    return;
                } else {
                    std::fprintf(stderr, "tinyluminaria: composite: %s\n",
                                 px.error().message.c_str());
                }
            }
            (void)fe.output.commit(kBg);
        }));
    });

    // Track each new window; focus and mark it mapped on map.
    auto new_toplevel = shell.new_toplevel().connect([&](luminaria::NewToplevel& e) {
        Window& w = windows.emplace_back();
        w.toplevel = &e.toplevel;
        const int n = static_cast<int>(windows.size()) - 1;
        w.x = 40 + 30 * n;
        w.y = 40 + 30 * n;
        w.on_map = e.toplevel.map.connect([&w, &seat](luminaria::ToplevelMap&) {
            w.mapped = true;
            seat.set_keyboard_focus(&w.toplevel->surface());
        });
        w.on_destroy = e.toplevel.destroy.connect([&w](luminaria::ToplevelDestroy&) {
            w.mapped = false;
            w.toplevel = nullptr;
        });
    });

    // Route parent (KDE) input into our seat when running nested. Hit-test the
    // cursor against mapped windows; translate to surface-local coords.
    // NOTE: no cursor rendering yet. Keys, clicks, and modifiers (Shift/Ctrl) flow.
    luminaria::Signal<luminaria::PointerMotionAbsEvent>::Connection on_ptr_motion;
    luminaria::Signal<luminaria::PointerButtonEvent>::Connection on_ptr_button;
    luminaria::Signal<luminaria::KeyEvent>::Connection on_key;
    luminaria::Signal<luminaria::ModifiersEvent>::Connection on_mods;
    if (nested) {
        auto route = [&](double x, double y) {
            Window* hit = nullptr;
            for (Window& w : windows) { // later windows draw on top → topmost hit
                if (w.mapped && w.toplevel != nullptr && w.bw > 0 && x >= w.x && y >= w.y &&
                    x < w.x + w.bw && y < w.y + w.bh) {
                    hit = &w;
                }
            }
            if (hit == nullptr) {
                ptr_focus = nullptr;
                return;
            }
            const double lx = x - hit->x, ly = y - hit->y;
            if (hit != ptr_focus) {
                ptr_focus = hit;
                seat.pointer_enter(hit->toplevel->surface(), lx, ly);
            } else {
                seat.pointer_motion(lx, ly);
            }
        };
        on_ptr_motion = nested->pointer_motion.connect([&](luminaria::PointerMotionAbsEvent& e) {
            if (e.x < 0) { // pointer left our window
                ptr_focus = nullptr;
            } else {
                route(e.x, e.y);
            }
        });
        on_ptr_button = nested->pointer_button.connect([&](luminaria::PointerButtonEvent& e) {
            if (ptr_focus != nullptr) {
                seat.set_keyboard_focus(&ptr_focus->toplevel->surface());
                seat.pointer_button(e.button, e.pressed);
            }
        });
        on_key = nested->key.connect(
            [&](luminaria::KeyEvent& e) { seat.notify_key(e.keycode, e.pressed); });
        on_mods = nested->modifiers.connect([&](luminaria::ModifiersEvent& e) {
            seat.notify_modifiers(e.depressed, e.latched, e.locked, e.group);
        });
    }

    if (auto socket = display.add_socket_auto()) {
        setenv("WAYLAND_DISPLAY", socket->c_str(), 1);
        std::printf("tinyluminaria running on WAYLAND_DISPLAY=%s\n", socket->c_str());
    }

    (void)backend->start();

    luminaria::EventSource exit_timer;
    if (const char* ms = std::getenv("LUMINARIA_EXIT_MS")) {
        exit_timer = display.event_loop().add_timer([&] { display.terminate(); });
        exit_timer.arm(static_cast<unsigned>(std::atoi(ms)));
    }

    display.run();
    return 0;
}
