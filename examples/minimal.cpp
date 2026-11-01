// minimal — the smallest compositor that is still usable, and the C++ twin of
// bindings/rust/examples/minimal.rs: every window fills the first output, newest
// on top. What is left here is what a compositor decides: where windows go, which
// one has the keyboard, what a key does. Logo+Return opens $TERMINAL (default foot),
// Logo+Tab brings the bottom window up, Logo+q closes the top one, Logo+Escape quits.
// LUMINARIA_BACKEND=auto|nested|drm|headless, LUMINARIA_DRM_DEVICE (or argv[1]),
// LUMINARIA_VRR=1, LUMINARIA_EXIT_MS and LUMINARIA_TRACE=1 say how it runs.
#include <xkbcommon/xkbcommon.h>

#include <csignal>
#include <cstdlib>

import luminaria.shell;
import minimal_trace;
import std;

namespace lm = luminaria;

int main(int argc, char** argv) {
    std::signal(SIGCHLD, SIG_IGN); // spawned terminals reap themselves
    auto options = lm::must(lm::backend_options_from_env(argc > 1 ? argv[1] : ""), "backend");
    options.title = "minimal";

    auto display = lm::must(lm::Display::create(), "display");
    auto globals = lm::must(lm::CoreGlobals::create(display), "globals");
    lm::XdgShell& shell = globals.shell();
    lm::Seat& seat = globals.seat();
    lm::OutputLayout layout;
    lm::PopupTree popups(shell);
    auto picked = lm::must(lm::AutoBackend::create(display.event_loop(), options), "backend");

    lm::SceneRenderer renderer(picked.vulkan_options());
    renderer.set_presentation(&globals.presentation());
    auto dmabuf = renderer.linux_dmabuf(display);
    lm::Cursor cursor(renderer, layout, seat, &globals.cursor_shape());

    std::vector<lm::Toplevel*> stack;                    // mapped windows, back to front
    lm::Box usable{0, 0, options.width, options.height}; // the first output: where windows go
    std::vector<lm::SceneItem> scene;                    // rebuilt whenever asked

    const char* vrr = std::getenv("LUMINARIA_VRR");
    lm::ScreenManager screens(display, picked.backend(), renderer, layout,
                              {.background = {0.12f, 0.13f, 0.16f, 1.0f},
                               .variable_refresh = vrr != nullptr && *vrr != '\0' && *vrr != '0',
                               .name = "minimal"});
    screens.follow_popups(popups);
    screens.set_scene_builder([&](lm::Output&) -> std::span<const lm::SceneItem> {
        scene.clear();
        for (lm::Toplevel* t : stack) {
            scene.push_back(lm::toplevel_item(*t, usable.x, usable.y));
        }
        popups.append_to(scene);
        return scene;
    });
    lm::DefaultInputRouter input(seat, cursor, screens, shell, popups);
    input.attach(picked);

    // The top window has the keyboard and the activated look.
    auto restack = [&] {
        shell.focus(seat, stack.empty() ? nullptr : stack.back());
        screens.invalidate();
    };
    auto raise = [&](lm::Toplevel* t) {
        std::erase(stack, t);
        stack.push_back(t);
        restack();
    };

    lm::Connections connections; // dropped before anything the callbacks capture
    connections += shell.new_toplevel().connect([&](lm::NewToplevel& e) {
        lm::Toplevel* t = &e.toplevel;
        t->set_maximized(true);
        t->configure(usable.size()); // before its first frame: it never shows at its own size
        connections += t->map.connect([&, t] {
            globals.fractional_scale().set_scale(t->surface(), layout.primary_scale());
            raise(t);
        });
        auto leave = [&, t] {
            std::erase(stack, t);
            restack();
        };
        connections += t->unmap.connect(leave);
        connections += t->destroy.connect(leave);
    });
    connections += layout.changed.connect([&] {
        if (!layout.empty()) {
            usable = layout.primary_box();
            shell.set_bounds(usable.size());
            for (lm::Toplevel* t : stack) {
                t->configure(usable.size());
            }
            screens.invalidate();
        }
    });

    input.bind(lm::Mod::logo, XKB_KEY_Return, [] { (void)std::system("${TERMINAL:-foot} &"); });
    input.bind(lm::Mod::logo, XKB_KEY_Tab, [&] {
        if (stack.size() > 1) {
            std::rotate(stack.begin(), stack.begin() + 1, stack.end());
            restack();
        }
    });
    input.bind(lm::Mod::logo, XKB_KEY_q, [&] {
        if (!stack.empty()) {
            stack.back()->close();
        }
    });
    input.bind(lm::Mod::logo, XKB_KEY_Escape, [&] { display.terminate(); });
    input.on_button([&](std::uint32_t, bool pressed, const lm::SceneHit& hit) {
        if (pressed && hit.tag != 0) {
            raise(reinterpret_cast<lm::Toplevel*>(hit.tag));
        }
    });
    connections += picked.close_requested().connect([&] { display.terminate(); });

    const std::string socket = lm::must(display.add_socket_auto(), "socket");
    setenv("WAYLAND_DISPLAY", socket.c_str(), 1); // for what Logo+Return spawns
    trace::Trace tracing(display, picked, renderer, layout, shell);
    lm::must(picked.start(), "backend");
    std::println("minimal: WAYLAND_DISPLAY={}", socket);
    lm::EventSource quit = display.event_loop().add_timer([&] { display.terminate(); });
    if (const char* ms = std::getenv("LUMINARIA_EXIT_MS"); ms != nullptr) {
        quit.arm(static_cast<unsigned>(std::atoi(ms)));
    }
    display.run();
}
