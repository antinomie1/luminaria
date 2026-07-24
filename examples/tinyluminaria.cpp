// tinyluminaria — a minimal reference compositor built on luminaria. Mirrors tinywl: wire
// up the backend, compositor, xdg-shell, seat, and scene, then run the loop.
//
// Runs on the headless backend (no GPU/display needed to smoke-test the wiring).
// Set LUMINARIA_EXIT_MS to auto-terminate after N ms (used by the smoke test).
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "luminaria/backend/headless.hpp"
#include "luminaria/backend/wayland.hpp"
#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"
#include "luminaria/scene.hpp"
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
    luminaria::Scene scene;

    // Keep every subscription alive for the process lifetime.
    std::vector<luminaria::Signal<luminaria::ToplevelMap>::Connection> map_conns;
    std::vector<luminaria::Signal<luminaria::FrameEvent>::Connection> frame_conns;

    auto new_output = backend->new_output.connect([&](luminaria::NewOutput& e) {
        frame_conns.push_back(e.output.frame.connect([](luminaria::FrameEvent& fe) {
            // Paint a background each frame. This keeps the parent frame-callback
            // chain alive on the nested backend and gives the window content.
            // TODO: solid fill; scene -> Vulkan compositing replaces it later.
            (void)fe.output.commit(luminaria::Color{0.1f, 0.1f, 0.12f, 1.0f});
        }));
    });

    // Each new window becomes a scene surface node; focus it on map.
    auto new_toplevel = shell.new_toplevel().connect([&](luminaria::NewToplevel& e) {
        scene.root().add_surface(e.toplevel.surface(), 0, 0);
        map_conns.push_back(e.toplevel.map.connect([&](luminaria::ToplevelMap& m) {
            seat.set_keyboard_focus(&m.toplevel.surface());
        }));
    });

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
