// luminaria-tty — a real bare-metal compositor. DRM output + libinput input +
// wl_compositor/xdg-shell/seat, compositing client windows on the GPU and
// scanning them out to the monitor.
//
// Run from a free VT with the desktop stopped (see README). It prints its
// WAYLAND_DISPLAY; point a client at it from another VT/ssh, e.g.:
//     WAYLAND_DISPLAY=wayland-1 weston-simple-shm
// Esc quits.
//
// Pointer input is hit-tested against the same layer list that gets rendered,
// and the cursor rides the KMS cursor plane. TODO: windows cascade at fixed
// offsets — no move/resize/stacking UI.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h> // close, for the fences handed between GPU and KMS

#include <drm_fourcc.h>

import luminaria;

namespace {
constexpr uint32_t KEY_ESC = 1;
// XRGB8888: opaque 32-bit, the one format every KMS primary plane scans out.
constexpr uint32_t kScanoutFormat = DRM_FORMAT_XRGB8888;

/// Modifiers both the GPU can render+export and the display can scan out.
/// Empty is fine — create_scanout() then falls back to LINEAR.
std::vector<std::uint64_t> usable_modifiers(luminaria::VulkanRenderer& renderer,
                                            luminaria::Output& output) {
    const std::vector<std::uint64_t> display = output.scanout_modifiers(kScanoutFormat);
    std::vector<std::uint64_t> both;
    for (std::uint64_t m : renderer.scanout_modifiers(kScanoutFormat)) {
        if (std::find(display.begin(), display.end(), m) != display.end()) {
            both.push_back(m);
        }
    }
    return both;
}

/// Small env knobs so a rotated or HiDPI panel can be tried without a config
/// file: LUMINARIA_SCALE=2, LUMINARIA_TRANSFORM=1 (90 degrees) .. 7.
int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }
    const int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

/// LUMINARIA_MODE=WxH or WxH@R (R in Hz, fractional allowed). A zero width
/// means "unset": keep the connector's preferred mode.
luminaria::OutputMode env_mode() {
    const char* spec = std::getenv("LUMINARIA_MODE");
    if (spec == nullptr) {
        return {};
    }
    int w = 0, h = 0;
    double hz = 0.0;
    const int fields = std::sscanf(spec, "%dx%d@%lf", &w, &h, &hz);
    if (fields < 2 || w <= 0 || h <= 0) {
        std::fprintf(stderr, "luminaria-tty: LUMINARIA_MODE: expected WxH or WxH@Hz\n");
        return {};
    }
    return luminaria::OutputMode{w, h, static_cast<int>(hz * 1000.0 + 0.5), false};
}

luminaria::Transform env_transform() {
    const int value = env_int("LUMINARIA_TRANSFORM", 0);
    return value >= 0 && value <= 7 ? static_cast<luminaria::Transform>(value)
                                    : luminaria::Transform::normal;
}

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

    // Before the Display: surfaces cache GPU textures owned by this renderer,
    // and locals are destroyed in reverse order.
    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "luminaria-tty: vulkan: %s\n", renderer.error().message.c_str());
        return 1;
    }

    auto display = luminaria::Display::create();
    if (!display) {
        return 1;
    }
    if (auto s = display->init_shm(); !s) {
        std::fprintf(stderr, "luminaria-tty: shm: %s\n", s.error().message.c_str());
    }

    // Join the seat first, if we can. With it, a VT switch is safe: DRM master
    // is handed back, input is suspended, and the modeset is re-applied on the
    // way in. Without it everything still works from a logged-in VT — until
    // someone presses Ctrl+Alt+F2.
    std::optional<luminaria::Session> session;
    if (auto s = luminaria::Session::create(display->event_loop())) {
        session.emplace(std::move(*s));
        std::printf("luminaria-tty: session = libseat (VT switching enabled)\n");
    } else {
        std::printf("luminaria-tty: session = none (%s); VT switching unsafe\n",
                    s.error().message.c_str());
    }
    luminaria::Session* seat_session = session.has_value() ? &*session : nullptr;

    auto drm = device != nullptr
                   ? luminaria::DrmBackend::create(display->event_loop(), device, seat_session)
                   : luminaria::DrmBackend::create(display->event_loop(), seat_session);
    if (!drm) {
        std::fprintf(stderr, "luminaria-tty: drm: %s\n", drm.error().message.c_str());
        return 1;
    }
    auto input = luminaria::LibinputBackend::create(display->event_loop(), seat_session);
    if (!input) {
        std::fprintf(stderr, "luminaria-tty: input: %s\n", input.error().message.c_str());
        return 1;
    }

    auto compositor = luminaria::Compositor::create(*display);
    auto shell = luminaria::XdgShell::create(*display);
    auto seat = luminaria::Seat::create(*display);
    auto single_pixel = luminaria::SinglePixelBufferManager::create(*display);
    if (!compositor || !shell || !seat || !single_pixel) {
        std::fprintf(stderr, "luminaria-tty: protocol setup failed\n");
        return 1;
    }

    // GPU clients (GTK4, anything on Mesa/EGL) hand us dmabufs; the renderer
    // imports them straight into the composite with no copy.
    auto dmabuf = luminaria::LinuxDmabuf::create(*display, &*renderer);
    if (!dmabuf) {
        std::fprintf(stderr, "luminaria-tty: linux-dmabuf: %s\n", dmabuf.error().message.c_str());
    }

    // Frame pacing and animation timing, fed from Output::present.
    auto presentation = luminaria::Presentation::create(*display);
    auto tearing = luminaria::TearingControlManager::create(*display);
    // Named cursors: the client says "text" or "ns-resize", we look it up in the
    // theme and hand the picture to the cursor plane.
    auto cursor_shape = luminaria::CursorShapeManager::create(*display);
    if (!presentation || !tearing || !cursor_shape) {
        std::fprintf(stderr, "luminaria-tty: presentation/tearing/cursor setup failed\n");
        return 1;
    }

    std::list<Window> windows; // stable addresses; slots kept for the process life
    // Set once the screens exist; map/unmap call it to force a full repaint.
    std::function<void()> window_changed = [] {};

    auto new_toplevel = shell->new_toplevel().connect([&](luminaria::NewToplevel& e) {
        Window& w = windows.emplace_back();
        w.toplevel = &e.toplevel;
        const int n = static_cast<int>(windows.size()) - 1;
        w.x = 40 + 30 * n;
        w.y = 40 + 30 * n;
        w.on_map = e.toplevel.map.connect([&w, &seat, &window_changed](luminaria::ToplevelMap&) {
            w.mapped = true;
            seat->set_keyboard_focus(&w.toplevel->surface());
            seat->pointer_enter(w.toplevel->surface(), 0, 0);
            window_changed();
        });
        w.on_destroy =
            e.toplevel.destroy.connect([&w, &window_changed](luminaria::ToplevelDestroy&) {
                w.mapped = false;
                w.toplevel = nullptr;
                window_changed();
            });
    });

    // One z-ordered list per frame, used for BOTH rendering and hit-testing, so
    // a click can never land somewhere the pixels aren't. Positions are in
    // layout coordinates; each window contributes its whole subsurface tree.
    struct Layer {
        luminaria::Surface* surface;
        int x, y;
    };
    auto build_layers = [&windows] {
        std::vector<Layer> layers;
        for (Window& w : windows) {
            if (!w.mapped || w.toplevel == nullptr) {
                continue;
            }
            for (const luminaria::SurfaceAt& at : w.toplevel->surface().surface_tree()) {
                layers.push_back(Layer{at.surface, w.x + at.x, w.y + at.y});
            }
        }
        return layers;
    };
    // Every connected monitor becomes a Screen: its own wl_output, its own pair
    // of GPU scanout buffers, its own damage history.
    struct Screen {
        luminaria::Output* output = nullptr;
        std::unique_ptr<luminaria::OutputGlobal> global;
        std::vector<luminaria::ScanoutTarget> targets;
        std::vector<std::uint32_t> ids;
        std::optional<luminaria::DirectScanout> direct;
        int back = 0;
        bool needs_full_redraw = true;
        std::vector<luminaria::Box> previous_damage; // the other buffer's debt
        luminaria::Signal<luminaria::FrameEvent>::Connection frame_conn;
        luminaria::Signal<luminaria::PresentEvent>::Connection present_conn;
        luminaria::Signal<luminaria::OutputDestroy>::Connection destroy_conn;
        luminaria::Signal<luminaria::OutputModeChange>::Connection mode_conn;
    };
    std::list<Screen> screens; // stable addresses: the callbacks capture Screen&
    luminaria::OutputLayout layout;

    // A window appearing or vanishing changes pixels nobody reported damage for.
    auto damage_everything = [&screens] {
        for (Screen& sc : screens) {
            sc.needs_full_redraw = true;
        }
    };
    window_changed = damage_everything;

    // (Re)build everything that is sized for an output's current mode: the pair
    // of GPU scanout buffers, the direct-scanout cache, its box in the layout
    // and what its wl_output tells clients. Run when the output appears, and
    // again every time its mode changes — after a switch, every one of those is
    // describing a resolution that is no longer on the wire.
    auto rebuild_screen = [&](Screen& screen) {
        luminaria::Output& output = *screen.output;
        const int ow = output.width();
        const int oh = output.height();

        screen.targets.clear();
        screen.ids.clear();
        if (screen.direct.has_value()) {
            screen.direct->clear();
        } else {
            screen.direct.emplace(output);
        }
        screen.back = 0;
        screen.previous_damage.clear();
        screen.needs_full_redraw = true;

        // Double-buffered GPU scanout: composite into a dmabuf the GPU renders
        // to and KMS scans out. Nothing on this path travels through the CPU.
        const std::vector<std::uint64_t> mods = usable_modifiers(*renderer, output);
        while (screen.targets.size() < 2) {
            auto target = renderer->create_scanout(ow, oh, kScanoutFormat, mods);
            if (!target) {
                std::fprintf(stderr, "luminaria-tty: scanout: %s\n",
                             target.error().message.c_str());
                break;
            }
            auto id = output.import_scanout(target->plane());
            if (!id) {
                std::fprintf(stderr, "luminaria-tty: scanout import: %s\n",
                             id.error().message.c_str());
                break;
            }
            screen.targets.push_back(std::move(*target));
            screen.ids.push_back(*id);
        }
        if (screen.targets.size() < 2) {
            std::fprintf(stderr, "luminaria-tty: falling back to CPU read-back scanout\n");
            screen.targets.clear();
            screen.ids.clear();
        }

        // The output occupies a different amount of the layout at a new mode,
        // so its place in it is recomputed rather than kept.
        layout.remove(output);
        layout.add_auto(output);
        const luminaria::Box view = layout.box_of(output);
        if (screen.global) {
            screen.global->set_modes(output.modes());
            screen.global->set_mode(ow, oh, output.current_mode().refresh_mhz);
            screen.global->set_scale(output.scale());
            screen.global->set_transform(output.transform());
            screen.global->set_logical_position(view.x, view.y);
        }
        std::printf("luminaria-tty: output %dx%d@%.3gHz at (%d,%d)\n", ow, oh,
                    output.current_mode().refresh_mhz / 1000.0, view.x, view.y);
    };

    auto out_conn = drm->new_output.connect([&](luminaria::NewOutput& e) {
        Screen& screen = screens.emplace_back();
        screen.output = &e.output;
        // Panel rotation, HiDPI and the mode are compositor policy; here they
        // come from the environment so the paths can be exercised without a
        // config file. LUMINARIA_MODE=1920x1080 or =1920x1080@60.
        e.output.set_scale(env_int("LUMINARIA_SCALE", 1));
        e.output.set_transform(env_transform());
        if (const luminaria::OutputMode want = env_mode(); want.width > 0) {
            if (auto s = e.output.set_mode(want.width, want.height, want.refresh_mhz); !s) {
                std::fprintf(stderr, "luminaria-tty: LUMINARIA_MODE: %s\n",
                             s.error().message.c_str());
                for (const luminaria::OutputMode& m : e.output.modes()) {
                    std::fprintf(stderr, "  available: %dx%d@%.3gHz%s\n", m.width, m.height,
                                 m.refresh_mhz / 1000.0, m.preferred ? " (preferred)" : "");
                }
            }
        }

        if (auto og = luminaria::OutputGlobal::create(*display, e.output.width(),
                                                      e.output.height(),
                                                      "luminaria-" + std::to_string(screens.size()))) {
            screen.global = std::make_unique<luminaria::OutputGlobal>(std::move(*og));
        }

        // Unplugged: drop everything hanging off this output. The Output is
        // already half-destroyed here, so we may only compare its address.
        screen.destroy_conn =
            e.output.destroy.connect([&screens, &layout, out = &e.output](luminaria::OutputDestroy&) {
                std::printf("luminaria-tty: output removed\n");
                layout.remove(*out);
                std::erase_if(screens, [out](const Screen& sc) { return sc.output == out; });
            });

        // A mode switch invalidates every buffer we sized for the old one, and
        // the backend has already dropped its side of them.
        screen.mode_conn = e.output.mode_changed.connect(
            [&rebuild_screen, &screen = screen](luminaria::OutputModeChange&) {
                rebuild_screen(screen);
            });

        rebuild_screen(screen);

        // A frame is on screen: pace the clients that drew it, and answer their
        // presentation feedback with the vblank timestamp the kernel gave us.
        screen.present_conn = e.output.present.connect([&, &screen = screen]
                                                       (luminaria::PresentEvent& pe) {
            // The flip landed, so whatever a direct scanout replaced is off the
            // screen and its client can have it back.
            if (screen.direct.has_value()) {
                screen.direct->presented();
            }
            for (Window& w : windows) {
                if (!w.mapped || w.toplevel == nullptr) {
                    continue;
                }
                luminaria::Surface& surface = w.toplevel->surface();
                surface.send_frame_done(pe.time_ms());
                presentation->notify_presented(surface, pe);
            }
        });

        screen.frame_conn = e.output.frame.connect([&, &screen = screen](luminaria::FrameEvent& fe) {
            constexpr luminaria::Color background{0.1f, 0.1f, 0.13f, 1.0f};
            const luminaria::Box view_box = layout.box_of(fe.output);

            // --- direct scanout -------------------------------------------
            //
            // One window, covering the whole monitor, handing us a buffer the
            // display can already read: point the CRTC at it and draw nothing
            // at all. This is the fullscreen video / game case, and it saves
            // the entire composite pass plus the memory bandwidth of a
            // full-screen blit every frame.
            //
            // The cursor has to be on its own plane, or it would not appear:
            // nothing is compositing it in.
            if (screen.direct.has_value() && fe.output.has_cursor_plane()) {
                const std::vector<Layer> layers = build_layers();
                if (layers.size() == 1 && layers[0].x == view_box.x &&
                    layers[0].y == view_box.y &&
                    layers[0].surface->surface_width() == fe.output.logical_width() &&
                    layers[0].surface->surface_height() == fe.output.logical_height()) {
                    luminaria::Surface& surface = *layers[0].surface;
                    if (auto id = screen.direct->id_for(surface)) {
                        // The client's own acquire fence goes straight to KMS:
                        // the flip waits for the client's GPU work, and we
                        // never touch it.
                        int fence = surface.acquire_fence_fd();
                        if (fence >= 0) {
                            fence = dup(fence);
                        }
                        if (fe.output.commit_scanout(*id, fence)) {
                            screen.direct->committed(surface);
                            surface.clear_damage();
                            // The composited buffers no longer hold this frame,
                            // so the next composited frame must be complete.
                            screen.needs_full_redraw = true;
                            return;
                        }
                    }
                }
            }

            // Textures live on the surfaces and are cached there; this is a
            // list of borrowed pointers.
            std::vector<luminaria::GpuTextureFill> textures;
            std::vector<luminaria::Surface*> drawn;
            std::vector<luminaria::Box> damage;
            std::vector<int> acquire_fences; // explicit-sync clients, borrowed fds
            bool wants_tearing = false;
            for (const Layer& layer : build_layers()) {
                luminaria::Surface& surface = *layer.surface;
                luminaria::GpuTexture* texture = surface.buffer_texture(*renderer);
                if (texture == nullptr) {
                    continue;
                }
                // Layer position is in layout space; the target is one output.
                const int x = layer.x - view_box.x;
                const int y = layer.y - view_box.y;
                for (const luminaria::Box& d : surface.damage()) {
                    damage.push_back(luminaria::Box{x + d.x, y + d.y, d.width, d.height});
                }
                wants_tearing = wants_tearing || surface.tearing_hint();
                if (const int fence = surface.acquire_fence_fd(); fence >= 0) {
                    acquire_fences.push_back(fence);
                }
                drawn.push_back(&surface);

                luminaria::GpuTextureFill fill{};
                fill.texture = texture;
                fill.x = x;
                fill.y = y;
                // Surface coordinates, not buffer pixels: a 2x HiDPI client
                // still occupies its 1x window.
                fill.w = surface.surface_width();
                fill.h = surface.surface_height();
                fill.transform = surface.buffer_transform();
                surface.buffer_source_uv(fill.u0, fill.v0, fill.u1, fill.v1);
                // What the client promised is opaque, so the renderer can skip
                // everything underneath it.
                fill.opaque = surface.opaque_region();
                fill.opaque.translate(x, y);
                textures.push_back(fill);
            }
            fe.output.set_tearing(wants_tearing);

            // The buffer we are about to draw into is two frames old, so it owes
            // the previous frame's damage as well as this one's.
            std::vector<luminaria::Box> repaint;
            if (!screen.needs_full_redraw) {
                repaint = damage;
                repaint.insert(repaint.end(), screen.previous_damage.begin(),
                               screen.previous_damage.end());
                if (repaint.empty()) {
                    repaint.push_back(luminaria::Box{}); // nothing at all changed
                }
            }
            screen.previous_damage = damage;
            screen.needs_full_redraw = false;

            bool presented = false;
            if (!screen.targets.empty()) {
                luminaria::ScanoutTarget& target = screen.targets[screen.back];
                const luminaria::OutputMapping mapping{screen.output->transform(),
                                                       screen.output->scale()};
                // The buffer we are about to draw into may still be on screen;
                // the flip's out-fence says when it isn't. Waiting for it on the
                // GPU is the point — nothing here blocks.
                target.set_acquire_fence(screen.output->take_present_fence());
                int render_fence = -1;
                const luminaria::RenderSync sync{acquire_fences, &render_fence};
                if (renderer->render_to(target, background, {}, textures, repaint, mapping,
                                        sync)) {
                    // Explicit-sync clients get the render's own fence as their
                    // release point: they may reuse the buffer the moment the
                    // GPU stops reading it, not when we next get round to it.
                    for (luminaria::Surface* s : drawn) {
                        s->notify_rendered(render_fence);
                    }
                    // commit_scanout takes the fence: KMS holds the flip until
                    // the GPU signals, so we never wait for the render either.
                    presented = screen.output->commit_scanout(screen.ids[screen.back],
                                                              render_fence).has_value();
                    render_fence = -1; // owned by commit_scanout now
                    if (presented) {
                        screen.back = 1 - screen.back;
                    }
                } else if (render_fence >= 0) {
                    close(render_fence);
                }
            }
            if (!presented) {
                (void)fe.output.commit(background);
            }
            for (luminaria::Surface* s : drawn) {
                s->clear_damage();
            }
        });
    });

    // --- libinput -> seat -----------------------------------------------------
    //
    // libinput reports relative motion; the compositor owns the cursor position.
    // It lives in layout coordinates so it can cross between monitors, and the
    // hit test runs against the same layer list the renderer draws.
    double cursor_x = 0, cursor_y = 0;
    bool cursor_placed = false;
    // Real cursor images from the user's theme, put on the KMS cursor plane.
    // That plane is why the pointer can move at input rate without recompositing
    // the screen behind it — the display hardware overlays it for free.
    auto cursor_theme = luminaria::CursorTheme::load();
    const char* cursor_name = "default";
    const char* cursor_uploaded = nullptr;
    auto push_cursor_image = [&] {
        if (!cursor_theme) {
            return;
        }
        const luminaria::CursorImage* image = cursor_theme->frame(cursor_name, 0);
        if (image == nullptr || cursor_uploaded == cursor_name) {
            return;
        }
        for (Screen& sc : screens) {
            if (sc.output->has_cursor_plane()) {
                (void)sc.output->set_cursor(image->rgba, image->width, image->height,
                                            image->hotspot_x, image->hotspot_y);
            }
        }
        cursor_uploaded = cursor_name;
    };
    // A client asking for a named shape (wp_cursor_shape_v1) just changes which
    // theme image we upload.
    auto cursor_shape_conn =
        cursor_shape->request().connect([&](luminaria::CursorShapeRequest& r) {
            cursor_name = r.name;
            push_cursor_image();
        });
    auto clamp_cursor = [&] {
        const luminaria::Box bounds = layout.bounds();
        if (bounds.empty()) {
            return;
        }
        if (!cursor_placed) {
            cursor_x = bounds.x + bounds.width / 2.0;
            cursor_y = bounds.y + bounds.height / 2.0;
            cursor_placed = true;
        }
        cursor_x = std::clamp(cursor_x, static_cast<double>(bounds.x),
                              static_cast<double>(bounds.x + bounds.width) - 1.0);
        cursor_y = std::clamp(cursor_y, static_cast<double>(bounds.y),
                              static_cast<double>(bounds.y + bounds.height) - 1.0);
    };
    // Topmost surface accepting input at (x,y), plus the point in its own
    // coordinates. accepts_input honours the client's input region.
    auto hit_test = [&](double x, double y, double& lx, double& ly) -> luminaria::Surface* {
        const std::vector<Layer> layers = build_layers();
        for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
            if (it->surface->accepts_input(x - it->x, y - it->y)) {
                lx = x - it->x;
                ly = y - it->y;
                return it->surface;
            }
        }
        return nullptr;
    };
    auto window_of = [&windows](luminaria::Surface* surface) -> Window* {
        for (Window& w : windows) {
            if (!w.mapped || w.toplevel == nullptr) {
                continue;
            }
            for (const luminaria::SurfaceAt& at : w.toplevel->surface().surface_tree()) {
                if (at.surface == surface) {
                    return &w;
                }
            }
        }
        return nullptr;
    };
    luminaria::Surface* pointer_focus = nullptr;
    auto deliver_motion = [&] {
        clamp_cursor();
        // Hardware cursor: one small atomic commit per output, no repaint.
        push_cursor_image();
        for (Screen& sc : screens) {
            if (!sc.output->has_cursor_plane()) {
                continue;
            }
            const luminaria::Box view = layout.box_of(*sc.output);
            const int lx_out = static_cast<int>(cursor_x) - view.x;
            const int ly_out = static_cast<int>(cursor_y) - view.y;
            if (view.contains(static_cast<int>(cursor_x), static_cast<int>(cursor_y))) {
                (void)sc.output->move_cursor(lx_out, ly_out);
            } else {
                (void)sc.output->hide_cursor();
            }
        }
        double lx = 0, ly = 0;
        luminaria::Surface* hit = hit_test(cursor_x, cursor_y, lx, ly);
        if (hit == nullptr) {
            if (pointer_focus != nullptr) {
                seat->pointer_clear_focus();
                pointer_focus = nullptr;
            }
            return;
        }
        if (hit != pointer_focus) {
            seat->pointer_enter(*hit, lx, ly);
            pointer_focus = hit;
        } else {
            seat->pointer_motion(lx, ly);
        }
    };

    auto key_conn = input->key().connect([&](luminaria::KeyEvent& e) {
        if (e.pressed && e.keycode == KEY_ESC) {
            display->terminate();
        } else {
            seat->notify_key(e.keycode, e.pressed);
        }
    });
    auto motion_conn = input->pointer_motion().connect([&](luminaria::PointerMotionEvent& e) {
        cursor_x += e.dx;
        cursor_y += e.dy;
        deliver_motion();
    });
    auto btn_conn = input->pointer_button().connect([&](luminaria::PointerButtonEvent& e) {
        // A press on a window raises keyboard focus with it, the way every
        // click-to-focus desktop behaves.
        if (e.pressed && pointer_focus != nullptr) {
            if (Window* w = window_of(pointer_focus); w != nullptr) {
                seat->set_keyboard_focus(&w->toplevel->surface());
            }
        }
        seat->pointer_button(e.button, e.pressed);
    });
    // A surface that dies while the pointer is over it must not stay cached.
    auto pointer_focus_conn =
        seat->pointer_focus_changed().connect([&](luminaria::SeatPointerFocus& e) {
            if (e.surface == nullptr) {
                pointer_focus = nullptr;
            }
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
