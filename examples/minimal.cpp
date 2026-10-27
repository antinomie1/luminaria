// minimal — the smallest compositor that is still usable, and the C++ twin of
// bindings/rust/examples/minimal.rs: every window is maximized onto the
// output, the newest on top, like a phone. Clicking a window raises it.
//
//   ./build/.../minimal                              # nested in the current session
//   LUMINARIA_BACKEND=drm ./build/.../minimal        # bare metal, from a free VT
//   LUMINARIA_BACKEND=headless LUMINARIA_EXIT_MS=500 ./build/.../minimal
//
// LUMINARIA_BACKEND is auto (nested, then DRM when a libseat session can be
// had, then headless) | nested | drm | headless. On DRM the card is argv[1] or
// LUMINARIA_DRM_DEVICE, else the first one with a monitor; Ctrl+Alt+Fn switches
// VTs. Logo+Return opens $TERMINAL (default foot), Logo+Tab brings the bottom
// window to the top, Logo+q closes the top one, Logo+Escape quits.
// LUMINARIA_TRACE=1 logs what scripts/tty-check.sh reads (trace.cppm).
//
// What is left here is what a compositor decides: where windows go, which one
// has the keyboard, what a key does.
#include <xkbcommon/xkbcommon.h>
#include <spawn.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>

import luminaria.shell;
import minimal_trace;
import std;

extern char** environ;

namespace {

namespace lm = luminaria;

template <class T>
T must(lm::Result<T> r, const char* what) {
    if (!r) {
        std::println(std::cerr, "minimal: {}: {}", what, r.error().message);
        std::exit(1);
    }
    return std::move(*r);
}

void must(const lm::Status& s, const char* what) {
    if (!s) {
        std::println(std::cerr, "minimal: {}: {}", what, s.error().message);
        std::exit(1);
    }
}

// Shift, Control, Alt (Mod1), Logo (Mod4) in xkb's core modifier bits. Caps
// and Num Lock are left out on purpose: a binding must not stop working
// because Caps Lock is on.
constexpr std::uint32_t kBindableMods = (1u << 0) | (1u << 2) | (1u << 3) | (1u << 6);
constexpr std::uint32_t kLogo = 1u << 6;

enum class Action { spawn, cycle, close, quit };
struct Binding {
    std::uint32_t mods;
    xkb_keysym_t keysym;
    Action action;
};
constexpr Binding kBindings[] = {{kLogo, XKB_KEY_Return, Action::spawn},
                                 {kLogo, XKB_KEY_Tab, Action::cycle},
                                 {kLogo, XKB_KEY_q, Action::close},
                                 {kLogo, XKB_KEY_Escape, Action::quit}};

struct Screen {
    lm::SceneOutput state;
    std::optional<lm::OutputGlobal> global;
};

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // read through a pipe by tty-check.sh
    std::signal(SIGCHLD, SIG_IGN);            // spawned terminals reap themselves
    lm::BackendOptions backend_options{.title = "minimal"};
    if (const char* want = std::getenv("LUMINARIA_BACKEND"); want != nullptr) {
        backend_options.kind = must(lm::parse_backend_kind(want), "LUMINARIA_BACKEND");
    }
    if (const char* device = argc > 1 ? argv[1] : std::getenv("LUMINARIA_DRM_DEVICE");
        device != nullptr) {
        backend_options.drm_device = device;
    }

    // Declaration order is destruction order, reversed: the globals die after
    // everything that uses them and the backend before them; the renderer
    // outlives the cursor, the dmabuf global and every output's state, which
    // cache its textures.
    auto display = must(lm::Display::create(), "display");
    auto globals = must(lm::CoreGlobals::create(display), "globals");
    lm::XdgShell& shell = globals.shell();
    lm::Seat& seat = globals.seat();
    lm::OutputLayout layout;
    lm::PopupTree popups(shell);

    // Nested, else DRM when a libseat session can be had, else headless; or
    // the one LUMINARIA_BACKEND names.
    auto picked = must(lm::AutoBackend::create(display.event_loop(), backend_options), "backend");
    lm::Session* session = picked.session();

    lm::SceneRenderer renderer(picked.vulkan_options());
    renderer.set_presentation(&globals.presentation());
    std::optional<lm::LinuxDmabuf> dmabuf; // GPU clients hand over dmabufs, imported with no copy
    if (renderer.gpu_enabled()) {
        dmabuf = lm::to_optional(lm::LinuxDmabuf::create(display, renderer.gpu()));
    }
    lm::Cursor cursor(renderer, layout, seat, &globals.cursor_shape());

    std::vector<lm::Toplevel*> stack; // mapped windows, back to front
    std::list<Screen> screens;
    lm::Box usable{0, 0, 1280, 800}; // the first output: where windows go
    std::vector<lm::SceneItem> scene; // layout coordinates, rebuilt whenever asked
    // Every callback below, dropped together before anything they capture.
    lm::Connections connections;

    auto build_scene = [&] {
        scene.clear();
        for (lm::Toplevel* t : stack) {
            lm::Surface& root = t->surface();
            // The client's shadow sits outside its window geometry, so the
            // geometry, not the buffer, is what lands on the usable area.
            const lm::XdgGeometry g = t->geometry();
            const bool framed = g.width > 0 && g.height > 0;
            scene.push_back({.box = {usable.x, usable.y, framed ? g.width : root.surface_width(),
                                     framed ? g.height : root.surface_height()},
                             .surface = root.id(),
                             .x = usable.x - (framed ? g.x : 0),
                             .y = usable.y - (framed ? g.y : 0),
                             .tag = reinterpret_cast<std::uintptr_t>(t)});
        }
        popups.append_to(scene);
    };

    // Tell the client under the pointer; with a button held, the one that
    // took the press.
    auto pointer_focus = [&] {
        if (const lm::Pointer& p = cursor.pointer(); p.present()) {
            build_scene();
            lm::scene_route_pointer(seat, scene, p.x(), p.y());
        }
    };

    // Something no client reports changed: ask every output for a frame and
    // let the frame's diff work out what moved.
    auto schedule_all = [&] {
        for (const lm::OutputBox& on : layout.outputs()) {
            on.output->schedule_frame();
        }
    };

    // The stack changed: the top window gets the keyboard and the activated
    // look, and the pointer may be over something else now.
    auto restack = [&] {
        shell.focus(seat, stack.empty() ? nullptr : stack.back());
        schedule_all();
        pointer_focus();
    };
    auto raise = [&](lm::Toplevel* t) {
        std::erase(stack, t);
        stack.push_back(t);
        restack();
    };

    // --- windows -------------------------------------------------------------

    connections += shell.new_toplevel().connect([&](lm::NewToplevel& e) {
        lm::Toplevel* t = &e.toplevel;
        // Sized before its first frame, so it never shows at its own size.
        t->set_maximized(true);
        t->configure(usable.size());
        connections += t->map.connect([&, t] {
            // Render at the output's density: the integer hint and, for
            // clients that speak fractional-scale, the exact scale.
            if (const lm::Output* first = layout.primary(); first != nullptr) {
                globals.fractional_scale().set_scale(t->surface(), first->scale());
            }
            raise(t);
        });
        auto leave = [&, t] {
            std::erase(stack, t);
            restack();
        };
        connections += t->unmap.connect(leave);
        connections += t->destroy.connect(leave);
    });

    // Menus, tooltips and combo boxes: the tree follows the shell, drawn above
    // their parents and dismissed by a click outside them.
    connections += popups.changed.connect(schedule_all);
    // Where a popup's parent is and what screen it is on, so a menu near the
    // bottom edge flips up instead of hanging off it.
    shell.set_popup_constraint_query([&](lm::Surface& parent, lm::Box& parent_box,
                                         lm::Box& area) {
        build_scene();
        for (const lm::SceneItem& item : scene) {
            if (item.surface == parent.id()) {
                parent_box = item.box;
                const lm::Output* on = layout.at(item.box.x, item.box.y);
                area = on != nullptr ? layout.box_of(*on) : layout.bounds();
                return !area.empty();
            }
        }
        return false;
    });

    // --- outputs -------------------------------------------------------------

    // Windows fill the first output. The layout keeps every output's box —
    // arrivals, departures and mode changes alike — and tells us here.
    connections += layout.changed.connect([&] {
        if (layout.empty()) {
            return;
        }
        usable = layout.primary_box();
        shell.set_bounds(usable.size());
        for (lm::Toplevel* t : stack) {
            t->configure(usable.size());
        }
        schedule_all();
    });
    connections += picked.backend().new_output.connect([&](lm::NewOutput& e) {
        lm::Output& output = e.output;
        Screen& screen = screens.emplace_back();
        if (auto s = renderer.reset(screen.state, output); !s) {
            std::println(std::cerr, "minimal: render target: {}; compositing on the CPU",
                         s.error().message);
        }
        layout.add_auto(output);
        screen.global = lm::to_optional(lm::OutputGlobal::create(
            display, output, layout, std::format("minimal-{}", screens.size())));
        connections += output.frame.connect([&] {
            build_scene();
            const lm::Color background{0.12f, 0.13f, 0.16f, 1.0f};
            if (auto shown = renderer.present(screen.state, output, scene, background, layout);
                !shown) {
                std::println(std::cerr, "minimal: present: {}", shown.error().message);
            }
        });
        connections += output.destroy.connect([&] {
            screens.remove_if([&screen](const Screen& x) { return &x == &screen; });
        });
    });

    // --- input ---------------------------------------------------------------

    lm::KeyRouter keys;
    auto act = [&](Action action) {
        switch (action) {
        case Action::spawn: {
            const char* term = std::getenv("TERMINAL") != nullptr ? std::getenv("TERMINAL") : "foot";
            char* args[] = {const_cast<char*>(term), nullptr};
            pid_t pid = 0;
            if (const int err = posix_spawnp(&pid, term, nullptr, nullptr, args, environ); err) {
                std::println(std::cerr, "minimal: {}: {}", term, std::strerror(err));
            }
            break;
        }
        case Action::cycle:
            if (stack.size() > 1) {
                std::rotate(stack.begin(), stack.begin() + 1, stack.end());
                restack();
            }
            break;
        case Action::close:
            if (!stack.empty()) {
                stack.back()->close();
            }
            break;
        case Action::quit: display.terminate(); break;
        }
    };
    // A press the compositor keeps; everything else is the client's.
    auto binding = [&](const lm::KeyEvent& e) {
        if (session != nullptr && session->switch_vt_for(e.keysym)) {
            seat.release_keys(); // their releases arrive on the other VT
            return true;
        }
        const std::uint32_t mods = (e.modifiers.depressed | e.modifiers.latched) & kBindableMods;
        for (const Binding& b : kBindings) {
            if (b.mods == mods && b.keysym == xkb_keysym_to_lower(e.keysym)) {
                act(b.action);
                return true;
            }
        }
        return false;
    };
    auto button = [&](std::uint32_t code, bool pressed) {
        if (pressed) {
            popups.dismiss_grabs_outside(seat.pointer_focus()); // a click outside a menu closes it
            build_scene();
            const lm::Pointer& p = cursor.pointer();
            if (const lm::SceneHit hit = lm::scene_hit_test(scene, p.x(), p.y()); hit.tag != 0) {
                raise(reinterpret_cast<lm::Toplevel*>(hit.tag));
            }
        }
        if (seat.pointer_focus().valid()) {
            seat.pointer_button(code, pressed);
        }
        if (!pressed) {
            pointer_focus(); // an implicit grab may have ended; re-pick
        }
    };

    // One handler for either backend: they differ in which events arrive
    // (relative motion and touch from libinput, absolute motion and leave from
    // the nested window), not in how. No touch: nothing here routes it.
    if (lm::InputSource* source = picked.input(); source != nullptr) {
        seat.follow(*source);
        connections += source->input().connect([&](lm::InputEvent& event) {
            lm::match(
                event,
                [&](const lm::KeyEvent& e) { keys.route(seat, e, binding); },
                [&](const lm::ModifiersEvent& e) { seat.notify_modifiers(e); },
                [&](const lm::PointerMotionEvent& e) {
                    if (cursor.move_by(e)) {
                        pointer_focus();
                    }
                },
                [&](const lm::PointerMotionAbsEvent& e) {
                    cursor.move_to(e);
                    pointer_focus();
                },
                [&](const lm::PointerLeaveEvent&) {
                    cursor.leave();
                    seat.pointer_clear_focus();
                },
                [&](const lm::PointerButtonEvent& e) { button(e.button, e.pressed); },
                [&](const lm::PointerAxisEvent& e) { seat.pointer_axis(e); },
                [](const auto&) {});
        });
    }
    if (lm::WaylandBackend* nested = picked.nested(); nested != nullptr) {
        connections += nested->close_requested.connect([&] { display.terminate(); });
    }
    if (session != nullptr) {
        connections += session->activity().connect([&](lm::SessionActive& e) {
            if (!e.active) {
                seat.release_keys();
            } else {
                schedule_all(); // repaint what the other VT drew over
            }
        });
    }

    // --- run -----------------------------------------------------------------

    const std::string socket = must(display.add_socket_auto(), "socket");
    setenv("WAYLAND_DISPLAY", socket.c_str(), 1); // for what Logo+Return spawns
    std::unique_ptr<trace::Trace> tracing;
    if (const char* on = std::getenv("LUMINARIA_TRACE"); on != nullptr && *on != '\0') {
        tracing = std::make_unique<trace::Trace>(display, picked, renderer, layout, shell);
    }
    must(picked.start(), "backend");
    std::println("minimal: WAYLAND_DISPLAY={} — Logo+Return opens a terminal, Logo+Escape quits",
                 socket);
    lm::EventSource exit_timer;
    if (const char* ms = std::getenv("LUMINARIA_EXIT_MS"); ms != nullptr) {
        exit_timer = display.event_loop().add_timer([&] { display.terminate(); });
        exit_timer.arm(static_cast<unsigned>(std::atoi(ms)));
    }
    display.run();
    return 0;
}
