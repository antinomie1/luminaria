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
//
// What is left here is what a compositor decides: where windows go, which one
// has the keyboard, what a key does. The pointer's picture (theme, cursor
// shapes, client sprites, the KMS cursor plane) is `Cursor`'s, and answering
// the display — frame callbacks, presentation feedback, the frame that turned
// out unchanged — is `SceneRenderer`'s.
#include <xkbcommon/xkbcommon.h>
#include <drm_fourcc.h>
#include <spawn.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

import luminaria.gpu;
import std;

extern char** environ;

namespace {

namespace lm = luminaria;

template <class E>
using Conn = typename lm::Signal<E>::Connection;

template <class... F>
struct Overloaded : F... {
    using F::operator()...;
};

[[noreturn]] void die(const char* what, const lm::Error& error) {
    std::fprintf(stderr, "minimal: %s: %s\n", what, error.message.c_str());
    std::exit(1);
}

template <class T>
T must(lm::Result<T> r, const char* what) {
    if (!r) {
        die(what, r.error());
    }
    return std::move(*r);
}

void must(const lm::Status& s, const char* what) {
    if (!s) {
        die(what, s.error());
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

struct Window {
    lm::Toplevel* toplevel = nullptr;
    Conn<lm::ToplevelMap> on_map;
    Conn<lm::ToplevelUnmap> on_unmap;
    Conn<lm::ToplevelDestroy> on_destroy;
};

struct PopupEntry {
    Conn<lm::PopupMap> on_map;
    Conn<lm::PopupUnmap> on_unmap;
    Conn<lm::PopupReposition> on_reposition;
    Conn<lm::PopupDestroy> on_destroy;
};

struct Screen {
    lm::Output* output = nullptr;
    lm::SceneOutput state;
    std::optional<lm::OutputGlobal> global;
    Conn<lm::FrameEvent> on_frame;
    Conn<lm::OutputModeChange> on_mode;
    Conn<lm::OutputDestroy> on_destroy;
};

// What the frames did, once a second on DRM and only when it changed: the
// evidence scripts/tty-check.sh reads. An idle screen asks for no frames at
// all, and then this line stops appearing — which is the point.
struct FrameCount {
    unsigned composited = 0, scanout = 0, unchanged = 0, cpu = 0;
    bool operator==(const FrameCount&) const noexcept = default;
};

} // namespace

int main(int argc, char** argv) {
    // Line-buffered: this is a diagnostic log, and a block-buffered one loses
    // its tail exactly when it matters — a crash, or a VT taken away.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::signal(SIGCHLD, SIG_IGN); // spawned terminals reap themselves
    lm::BackendOptions backend_options{.title = "minimal"};
    if (const char* want = std::getenv("LUMINARIA_BACKEND"); want != nullptr) {
        backend_options.kind = must(lm::parse_backend_kind(want), "LUMINARIA_BACKEND");
    }
    if (const char* device = argc > 1 ? argv[1] : std::getenv("LUMINARIA_DRM_DEVICE");
        device != nullptr) {
        backend_options.drm_device = device;
    }

    // Declaration order is destruction order, reversed. The renderer first, so
    // it dies last: every output state caches textures it owns. It is created
    // once the card is open, so it can be the GPU that card belongs to.
    std::optional<lm::SceneRenderer> renderer;
    auto display = must(lm::Display::create(), "display");
    must(display.init_shm(), "wl_shm");

    auto compositor = must(lm::Compositor::create(display), "wl_compositor");
    auto subcompositor = must(lm::Subcompositor::create(display), "wl_subcompositor");
    auto shell = must(lm::XdgShell::create(display), "xdg-shell");
    auto seat = must(lm::Seat::create(display), "wl_seat");
    auto data_device = must(lm::DataDeviceManager::create(display, seat), "data-device");
    auto single_pixel = must(lm::SinglePixelBufferManager::create(display), "single-pixel-buffer");
    auto presentation = must(lm::Presentation::create(display), "presentation-time");
    auto viewporter = must(lm::Viewporter::create(display), "wp_viewporter");
    auto fractional = must(lm::FractionalScaleManager::create(display), "fractional-scale");
    auto cursor_shape = must(lm::CursorShapeManager::create(display), "cursor-shape");
    // We draw no title bars, so clients draw their own.
    auto decoration = must(lm::XdgDecorationManager::create(display), "xdg-decoration");
    lm::OutputLayout layout;
    lm::PopupTree popups;

    // Backends after the globals: an output's last `present` must still find
    // the presentation global there.
    // Nested, else DRM when a libseat session can be had, else headless; or
    // the one LUMINARIA_BACKEND names. It owns the session, which outlives the
    // card and libinput opened through it.
    auto picked = must(lm::AutoBackend::create(display.event_loop(), backend_options), "backend");
    const bool on_drm = picked.kind() == lm::BackendKind::drm;
    lm::WaylandBackend* nested = picked.nested();
    lm::InputSource* source = picked.input(); // none headless
    lm::Session* session = picked.session();
    if (session != nullptr) {
        std::printf("minimal: session = libseat (VT switching enabled)\n");
    } else if (on_drm) {
        std::printf("minimal: session = none (%.*s); VT switching disabled\n",
                    static_cast<int>(picked.session_error().size()),
                    picked.session_error().data());
    }
    std::printf("minimal: backend = %.*s\n",
                static_cast<int>(lm::backend_kind_name(picked.kind()).size()),
                lm::backend_kind_name(picked.kind()).data());

    renderer.emplace(picked.vulkan_options());
    if (renderer->gpu_enabled()) {
        std::printf("minimal: renderer = %.*s\n",
                    static_cast<int>(renderer->gpu()->device_name().size()),
                    renderer->gpu()->device_name().data());
    } else {
        std::printf("minimal: renderer = CPU (%.*s)\n",
                    static_cast<int>(renderer->gpu_error().size()), renderer->gpu_error().data());
    }
    renderer->set_presentation(&presentation);
    // GPU clients hand over dmabufs, imported with no copy.
    std::optional<lm::LinuxDmabuf> dmabuf;
    if (renderer->gpu_enabled()) {
        if (auto d = lm::LinuxDmabuf::create(display, renderer->gpu()); d) {
            dmabuf.emplace(std::move(*d));
        }
    }
    lm::Cursor cursor(*renderer, layout, seat, &cursor_shape);

    // --- windows -------------------------------------------------------------

    std::list<Window> windows;
    std::list<PopupEntry> popup_entries;
    std::vector<Window*> stack; // mapped, back to front
    std::list<Screen> screens;
    lm::Box usable{0, 0, 1280, 800}; // the first output: where windows go
    lm::Toplevel* activated = nullptr;

    std::vector<lm::SceneItem> scene; // global coordinates, rebuilt whenever asked
    std::vector<lm::SceneItem> local; // `scene` in one output's coordinates
    std::vector<lm::SurfaceAt> tree;
    auto build_scene = [&] {
        scene.clear();
        for (Window* w : stack) {
            lm::Surface& root = w->toplevel->surface();
            // The client's shadow sits outside its window geometry, so the
            // geometry, not the buffer, is what lands on the usable area.
            const lm::XdgGeometry g = w->toplevel->geometry();
            const bool framed = g.width > 0 && g.height > 0;
            scene.push_back({.box = {usable.x, usable.y, framed ? g.width : root.surface_width(),
                                     framed ? g.height : root.surface_height()},
                             .surface = root.id(),
                             .x = usable.x - (framed ? g.x : 0),
                             .y = usable.y - (framed ? g.y : 0),
                             .tag = reinterpret_cast<std::uintptr_t>(w)});
        }
        popups.append_to(scene);
    };
    // Where `id` is drawn, subsurfaces included, for input that belongs to a
    // surface wherever the pointer is (a held button).
    auto origin_of = [&](lm::SurfaceId id, double& x, double& y) {
        for (const lm::SceneItem& item : scene) {
            lm::Surface* root = lm::surface_from_id(item.surface);
            if (root == nullptr) {
                continue;
            }
            tree.clear();
            root->surface_tree(tree);
            for (const lm::SurfaceAt& at : tree) {
                if (at.surface->id() == id) {
                    x = item.x + at.x;
                    y = item.y + at.y;
                    return true;
                }
            }
        }
        return false;
    };

    // Tell the client under the pointer. With a button held, every event goes
    // to the surface that took the press, in its own coordinates.
    auto pointer_focus = [&] {
        const lm::Pointer& p = cursor.pointer();
        if (!p.present()) {
            return;
        }
        build_scene();
        if (const lm::SurfaceId grab = seat.pointer_grab(); grab.valid() && !seat.dragging()) {
            double ox = 0, oy = 0;
            if (origin_of(grab, ox, oy)) {
                seat.pointer_motion(p.x() - ox, p.y() - oy);
            }
            return;
        }
        const lm::SceneHit hit = lm::scene_hit_test(scene, p.x(), p.y());
        if (hit.surface != nullptr) {
            seat.pointer_enter(*hit.surface, hit.sx, hit.sy);
        } else if (seat.pointer_focus().valid()) {
            seat.pointer_clear_focus();
        }
    };

    // Something no client reports changed: ask every output for a frame and
    // let the frame's diff work out what moved.
    auto schedule_all = [&] {
        for (Screen& s : screens) {
            s.output->schedule_frame();
        }
    };

    // The stack changed: the top window gets the keyboard and the activated
    // look, and the pointer may be over something else now.
    auto restack = [&] {
        lm::Toplevel* top = stack.empty() ? nullptr : stack.back()->toplevel;
        if (top != activated) {
            if (activated != nullptr) {
                activated->set_activated(false);
            }
            if (top != nullptr) {
                top->set_activated(true);
            }
            activated = top;
        }
        seat.set_keyboard_focus(top != nullptr ? &top->surface() : nullptr);
        schedule_all();
        pointer_focus();
    };
    auto raise = [&](Window* w) {
        std::erase(stack, w);
        stack.push_back(w);
        restack();
    };

    auto new_toplevel = shell.new_toplevel().connect([&](lm::NewToplevel& e) {
        Window& w = windows.emplace_back();
        w.toplevel = &e.toplevel;
        // Sized before its first frame, so it never shows at its own size.
        e.toplevel.set_maximized(true);
        (void)e.toplevel.configure(usable.width, usable.height);
        w.on_map = e.toplevel.map.connect([&](lm::ToplevelMap&) {
            if (!screens.empty()) {
                // Render at the output's density, both the integer hint and
                // the exact scale for clients that speak fractional-scale.
                lm::Surface& surface = w.toplevel->surface();
                const int scale = screens.front().output->scale();
                surface.set_preferred_buffer_scale(scale);
                fractional.set_scale(surface, scale * 120);
            }
            std::printf("minimal: window mapped — \"%s\"\n", w.toplevel->title().c_str());
            raise(&w);
        });
        w.on_unmap = e.toplevel.unmap.connect([&](lm::ToplevelUnmap&) {
            std::erase(stack, &w);
            restack();
        });
        // Erasing the entry inside its own slot is safe: a Signal never frees
        // the slot that is running, and nothing below touches `w` afterwards.
        w.on_destroy = e.toplevel.destroy.connect([&](lm::ToplevelDestroy&) {
            std::erase(stack, &w);
            if (activated == w.toplevel) {
                activated = nullptr;
            }
            windows.remove_if([&w](const Window& x) { return &x == &w; });
            restack();
        });
    });

    // Menus, tooltips and combo boxes: drawn above their parents, dismissed by
    // a click outside them.
    auto new_popup = shell.new_popup().connect([&](lm::NewPopup& e) {
        lm::Popup& popup = e.popup;
        popups.add(popup);
        PopupEntry& p = popup_entries.emplace_back();
        p.on_map = popup.map.connect([&](lm::PopupMap&) {
            popups.set_mapped(popup, true);
            schedule_all();
        });
        p.on_unmap = popup.unmap.connect([&](lm::PopupUnmap&) {
            popups.set_mapped(popup, false);
            schedule_all();
        });
        p.on_reposition = popup.reposition.connect([&](lm::PopupReposition&) { schedule_all(); });
        p.on_destroy = popup.destroy.connect([&](lm::PopupDestroy&) {
            popups.remove(popup);
            popup_entries.remove_if([&p](const PopupEntry& x) { return &x == &p; });
            schedule_all();
        });
    });
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

    FrameCount frames, reported;
    // Windows fill the first output; everything else about the layout is the
    // OutputLayout's.
    auto relayout = [&] {
        if (screens.empty()) {
            return;
        }
        usable = layout.box_of(*screens.front().output);
        shell.set_bounds(usable.width, usable.height);
        for (Window& w : windows) {
            (void)w.toplevel->configure(usable.width, usable.height);
        }
        cursor.refresh();
        schedule_all();
    };
    auto new_output = picked.backend().new_output.connect([&](lm::NewOutput& e) {
        lm::Output& output = e.output;
        Screen& screen = screens.emplace_back();
        screen.output = &output;
        layout.add_auto(output);
        if (auto s = renderer->reset(screen.state, output, DRM_FORMAT_XRGB8888); !s) {
            std::fprintf(stderr, "minimal: render target: %s; compositing on the CPU\n",
                         s.error().message.c_str());
        }
        const lm::Box box = layout.box_of(output);
        if (auto g = lm::OutputGlobal::create(display, output.width(), output.height(),
                                              "minimal-" + std::to_string(screens.size()));
            g) {
            screen.global.emplace(std::move(*g));
            screen.global->set_logical_position(box.x, box.y);
            screen.global->set_mode(output.width(), output.height(),
                                    output.current_mode().refresh_mhz);
            screen.global->set_scale(output.scale());
            screen.global->set_transform(output.transform());
        }
        std::printf("minimal: output %dx%d@%.3gHz at (%d,%d), scale %d, cursor plane %s\n",
                    output.width(), output.height(), output.current_mode().refresh_mhz / 1000.0,
                    box.x, box.y, output.scale(), output.has_cursor_plane() ? "yes" : "no");

        screen.on_frame = output.frame.connect([&](lm::FrameEvent&) {
            build_scene();
            const lm::Box view = layout.box_of(*screen.output);
            local.clear();
            for (lm::SceneItem item : scene) {
                item.box.x -= view.x;
                item.box.y -= view.y;
                item.x -= view.x;
                item.y -= view.y;
                local.push_back(item);
            }
            auto outcome = renderer->present(screen.state, *screen.output, local,
                                             lm::Color{0.12f, 0.13f, 0.16f, 1.0f});
            if (!outcome) {
                std::fprintf(stderr, "minimal: present: %s\n", outcome.error().message.c_str());
                return;
            }
            switch (*outcome) {
            case lm::SceneOutcome::composited: ++frames.composited; break;
            case lm::SceneOutcome::scanout: ++frames.scanout; break;
            case lm::SceneOutcome::unchanged: ++frames.unchanged; break;
            case lm::SceneOutcome::cpu: ++frames.cpu; break;
            }
        });
        // Everything sized for the old mode is rebuilt.
        screen.on_mode = output.mode_changed.connect([&](lm::OutputModeChange&) {
            (void)renderer->reset(screen.state, *screen.output, DRM_FORMAT_XRGB8888);
            const lm::Box old = layout.box_of(*screen.output);
            layout.add(*screen.output, old.x, old.y);
            if (screen.global.has_value()) {
                screen.global->set_mode(screen.output->width(), screen.output->height(),
                                        screen.output->current_mode().refresh_mhz);
            }
            relayout();
        });
        screen.on_destroy = output.destroy.connect([&](lm::OutputDestroy&) {
            std::printf("minimal: output removed\n");
            layout.remove(*screen.output);
            screens.remove_if([&screen](const Screen& x) { return &x == &screen; });
            relayout();
        });
        // libinput only reports deltas: the pointer starts mid-screen.
        if (on_drm && !cursor.pointer().present()) {
            cursor.move_to(box.x + box.width / 2.0, box.y + box.height / 2.0);
        }
        relayout();
    });

    // --- input ---------------------------------------------------------------

    lm::KeyRouter keys;
    std::vector<std::uint32_t> held; // keys a client was told are down
    auto release_held = [&] {
        for (const std::uint32_t k : held) {
            seat.notify_key(k, false);
        }
        held.clear();
        seat.notify_modifiers(0, 0, 0, 0);
    };
    auto act = [&](Action action) {
        switch (action) {
        case Action::spawn: {
            const char* term = std::getenv("TERMINAL") != nullptr ? std::getenv("TERMINAL") : "foot";
            char* args[] = {const_cast<char*>(term), nullptr};
            pid_t pid = 0;
            if (const int err = posix_spawnp(&pid, term, nullptr, nullptr, args, environ); err) {
                std::fprintf(stderr, "minimal: %s: %s\n", term, std::strerror(err));
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
                stack.back()->toplevel->close();
            }
            break;
        case Action::quit: display.terminate(); break;
        }
    };
    auto key = [&](const lm::KeyEvent& e) {
        if (!e.pressed) {
            if (keys.unconsume(e.keycode)) { // the release of a key we kept stays ours
                return;
            }
            std::erase(held, e.keycode);
            seat.notify_key(e.keycode, false);
            return;
        }
        // A press of a key we think we still hold: its release was lost, which
        // is what happens to the key that switched VTs away.
        (void)keys.unconsume(e.keycode);
        // Ctrl+Alt+Fn, which the keymap itself turns into XF86Switch_VT_n.
        if (session != nullptr && e.keysym >= XKB_KEY_XF86Switch_VT_1 &&
            e.keysym <= XKB_KEY_XF86Switch_VT_12) {
            keys.consume(e.keycode);
            release_held();
            (void)session->switch_to(static_cast<int>(e.keysym - XKB_KEY_XF86Switch_VT_1) + 1);
            return;
        }
        const std::uint32_t mods = (e.modifiers.depressed | e.modifiers.latched) & kBindableMods;
        for (const Binding& b : kBindings) {
            if (b.mods == mods && b.keysym == xkb_keysym_to_lower(e.keysym)) {
                keys.consume(e.keycode);
                act(b.action);
                return;
            }
        }
        held.push_back(e.keycode);
        seat.notify_key(e.keycode, true);
    };
    auto button = [&](std::uint32_t code, bool pressed) {
        if (pressed) {
            (void)popups.dismiss_grabs_outside(seat.pointer_focus()); // menu semantics
            build_scene();
            const lm::Pointer& p = cursor.pointer();
            if (const lm::SceneHit hit = lm::scene_hit_test(scene, p.x(), p.y()); hit.tag != 0) {
                raise(reinterpret_cast<Window*>(hit.tag));
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
    // the nested window), not in how.
    Conn<lm::InputEvent> on_input;
    if (source != nullptr) {
        on_input = source->input().connect([&](lm::InputEvent& event) {
            std::visit(
                Overloaded{
                    [&](const lm::KeyEvent& e) { key(e); },
                    [&](const lm::ModifiersEvent& e) { seat.notify_modifiers(e); },
                    // Clients get the layout the backend reads keys against.
                    [&](const lm::KeymapChange& e) { (void)seat.set_keymap(std::string(e.text)); },
                    [&](const lm::PointerMotionEvent& e) {
                        if (cursor.move_by(e.dx, e.dy)) {
                            pointer_focus();
                        }
                    },
                    [&](const lm::PointerMotionAbsEvent& e) {
                        const lm::Output* on = e.output != nullptr ? e.output
                                               : screens.empty()   ? nullptr
                                                                   : screens.front().output;
                        const lm::Box origin = on != nullptr ? layout.box_of(*on) : lm::Box{};
                        cursor.move_to(origin.x + e.x, origin.y + e.y);
                        pointer_focus();
                    },
                    [&](const lm::PointerLeaveEvent&) {
                        cursor.leave();
                        seat.pointer_clear_focus();
                    },
                    [&](const lm::PointerButtonEvent& e) { button(e.button, e.pressed); },
                    [&](const lm::PointerAxisEvent& e) { seat.pointer_axis(e); },
                    // A client that binds a wl_pointer no device can drive
                    // waits forever. No touch: nothing here routes it.
                    [&](const lm::InputCapabilities& caps) {
                        seat.set_capabilities(caps.keyboard, caps.pointer, false);
                        std::printf("minimal: input devices: keyboard=%s pointer=%s\n",
                                    caps.keyboard ? "yes" : "no", caps.pointer ? "yes" : "no");
                    },
                    [](const auto&) {}, // touch: no policy here
                },
                event);
        });
    }
    Conn<lm::HostCloseRequest> on_close;
    if (nested != nullptr) {
        on_close = nested->close_requested.connect([&](lm::HostCloseRequest&) { display.terminate(); });
    }
    Conn<lm::SessionActive> on_session;
    if (session != nullptr) {
        on_session = session->activity().connect([&](lm::SessionActive& e) {
            std::printf("minimal: session %s\n",
                        e.active ? "active — VT switched in" : "inactive — VT switched away");
            if (!e.active) {
                release_held();
            } else {
                schedule_all(); // repaint what the other VT drew over
            }
        });
    }

    // --- run -----------------------------------------------------------------

    const std::string socket = must(display.add_socket_auto(), "socket");
    setenv("WAYLAND_DISPLAY", socket.c_str(), 1); // for what Logo+Return spawns
    must(picked.start(), "backend");
    // What arrived before anything was listening: the nested keymap and seat
    // come with the connection, libinput's devices with start().
    if (source != nullptr) {
        if (!source->keymap().empty()) {
            (void)seat.set_keymap(source->keymap());
        }
        const lm::InputCapabilities caps = source->capabilities();
        seat.set_capabilities(caps.keyboard, caps.pointer, false);
    }
    std::printf("minimal: WAYLAND_DISPLAY=%s — Logo+Return opens a terminal, Logo+Escape "
                "quits\n",
                socket.c_str());

    lm::EventSource stats;
    if (on_drm) {
        stats = display.event_loop().add_timer([&] {
            if (!(frames == reported)) {
                std::printf("minimal: frames/s composited=%u scanout=%u unchanged=%u cpu=%u\n",
                            frames.composited - reported.composited,
                            frames.scanout - reported.scanout,
                            frames.unchanged - reported.unchanged, frames.cpu - reported.cpu);
                reported = frames;
            }
            stats.arm(1000);
        });
        stats.arm(1000);
    }
    lm::EventSource exit_timer;
    if (const char* ms = std::getenv("LUMINARIA_EXIT_MS"); ms != nullptr) {
        exit_timer = display.event_loop().add_timer([&] { display.terminate(); });
        exit_timer.arm(static_cast<unsigned>(std::atoi(ms)));
    }
    display.run();
    return 0;
}
