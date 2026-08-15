// luminaria-tty — a real bare-metal compositor. DRM output + libinput input +
// wl_compositor/xdg-shell/seat, compositing client windows on the GPU and
// scanning them out to the monitor.
//
// Run from a free VT with the desktop stopped (see README). It prints its
// WAYLAND_DISPLAY; point a client at it from another VT/ssh, e.g.:
//     WAYLAND_DISPLAY=wayland-1 weston-simple-shm
// Esc quits.
//
// Compositing, damage and the page flip are the shell layer's `Frame`; this
// file decides only where windows go. Pointer input is hit-tested against the
// same placement list that gets rendered, and the cursor rides the KMS cursor
// plane. TODO: windows cascade at fixed offsets — no move/resize/stacking UI.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <drm_fourcc.h>

import luminaria.gpu;

namespace {
constexpr uint32_t KEY_ESC = 1;
// XRGB8888: opaque 32-bit, the one format every KMS primary plane scans out.
constexpr uint32_t kScanoutFormat = DRM_FORMAT_XRGB8888;

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
    // Line-buffered: this output is a diagnostic log, and a block-buffered one
    // loses its tail exactly when it matters — a compositor that died, or a VT
    // that was taken away.
    setvbuf(stdout, nullptr, _IOLBF, 0);

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
    // A VT switch is the one thing on this path that touches DRM master, the
    // input fds and the modeset all at once, so it says so in the log: what
    // follows an "inactive" line is what a switch away actually did.
    luminaria::Signal<luminaria::SessionActive>::Connection session_conn;
    if (seat_session != nullptr) {
        session_conn = seat_session->activity().connect([](luminaria::SessionActive& e) {
            std::printf("luminaria-tty: session %s\n",
                        e.active ? "active — VT switched in" : "inactive — VT switched away");
        });
    }

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
            std::printf("luminaria-tty: window mapped at (%d,%d), %dx%d — \"%s\"\n", w.x, w.y,
                        w.toplevel->surface().surface_width(),
                        w.toplevel->surface().surface_height(), w.toplevel->title().c_str());
        });
        w.on_destroy =
            e.toplevel.destroy.connect([&w, &window_changed](luminaria::ToplevelDestroy&) {
                w.mapped = false;
                w.toplevel = nullptr;
                window_changed();
                std::printf("luminaria-tty: window gone\n");
            });
    });

    // Every connected monitor becomes a Screen: its own wl_output, and its own
    // Frame — the shell layer's ledger, holding this output's scanout buffers,
    // damage debt and direct-scanout cache.
    struct Screen {
        luminaria::Output* output = nullptr;
        std::unique_ptr<luminaria::OutputGlobal> global;
        std::optional<luminaria::Frame> frame;
        // Whether this screen's hardware is carrying the pointer. False means
        // the cursor is a placement in the frame like any other window.
        bool cursor_on_plane = false;
        luminaria::Signal<luminaria::FrameEvent>::Connection frame_conn;
        luminaria::Signal<luminaria::PresentEvent>::Connection present_conn;
        luminaria::Signal<luminaria::OutputDestroy>::Connection destroy_conn;
        luminaria::Signal<luminaria::OutputModeChange>::Connection mode_conn;
    };
    std::list<Screen> screens; // stable addresses: the callbacks capture Screen&
    luminaria::OutputLayout layout;

    // What the frames actually did, reported once a second and only when it
    // changed — this is what a real-hardware run leaves in the log. `scanout`
    // means a client's own buffer reached the CRTC untouched; a screen reporting
    // ~60 `unchanged` per second is the idle flipping that the next step is
    // meant to kill, and when it does, this line will simply stop appearing.
    struct FrameLog {
        unsigned composited = 0, scanout = 0, unchanged = 0, fallback = 0;

        void count(luminaria::Presented presented) {
            switch (presented) {
            case luminaria::Presented::composited: ++composited; break;
            case luminaria::Presented::scanout: ++scanout; break;
            case luminaria::Presented::unchanged: ++unchanged; break;
            case luminaria::Presented::fallback: ++fallback; break;
            }
        }
        bool operator==(const FrameLog&) const noexcept = default;
    };
    FrameLog frames;
    FrameLog reported;
    luminaria::EventSource stats_timer;
    stats_timer = display->event_loop().add_timer([&] {
        if (!(frames == reported)) {
            std::printf("luminaria-tty: frames/s composited=%u scanout=%u unchanged=%u "
                        "fallback=%u\n",
                        frames.composited - reported.composited,
                        frames.scanout - reported.scanout, frames.unchanged - reported.unchanged,
                        frames.fallback - reported.fallback);
            reported = frames;
        }
        stats_timer.arm(1000);
    });
    stats_timer.arm(1000);

    // --- the pointer's picture ------------------------------------------------
    //
    // Three states, and collapsing any two of them is visible on screen: the
    // focused client's own cursor surface, the theme image for "the client
    // hasn't said anything", and NOTHING for a client that asked for no pointer
    // (`Seat::cursor_hidden()`) — a fullscreen video player or a text editor
    // hiding the pointer while you type.
    //
    // The picture goes on the KMS cursor plane when the display has one: that is
    // what lets the pointer move at input rate without repainting the screen
    // behind it. When it doesn't, or when the image is bigger than the plane
    // (64x64 as a rule, and client sprites are not bound by that), it is
    // composited as an ordinary placement instead — the alternative is a
    // compositor with no visible pointer at all.
    double cursor_x = 0, cursor_y = 0;
    bool cursor_placed = false;
    luminaria::Output* cursor_prev_output = nullptr; // which screen last drew it
    auto cursor_theme = luminaria::CursorTheme::load();
    const char* cursor_name = "default";
    auto themed_cursor = [&]() -> const luminaria::CursorImage* {
        return cursor_theme ? cursor_theme->frame(cursor_name, 0) : nullptr;
    };
    // The theme image on the GPU, for screens that composite the cursor.
    // Uploaded when the shape changes, not per frame — the pointer moves
    // constantly but its picture almost never does.
    std::optional<luminaria::GpuTexture> cursor_texture;
    const char* cursor_texture_name = nullptr;
    const luminaria::CursorImage* cursor_texture_image = nullptr;
    auto ensure_cursor_texture = [&]() -> bool {
        const luminaria::CursorImage* image = themed_cursor();
        if (image == nullptr) {
            return false;
        }
        if (cursor_texture.has_value() && cursor_texture_name == cursor_name) {
            return true;
        }
        auto tex = renderer->upload_texture(image->width, image->height, image->rgba);
        if (!tex) {
            return false;
        }
        cursor_texture.emplace(std::move(*tex));
        cursor_texture_name = cursor_name;
        cursor_texture_image = image;
        return true;
    };
    std::vector<std::uint8_t> cursor_pixels; // client sprite, read back once per change

    // Refill one screen's placement list: one z-ordered pass over the windows,
    // each contributing its whole subsurface tree. This is the list the frame
    // both draws and hit-tests, so a click can never land somewhere the pixels
    // aren't — which is why it is rebuilt on input as well as on frame. It
    // allocates nothing once the vectors have grown.
    auto build_placements = [&](Screen& screen) {
        screen.frame->begin(layout.box_of(*screen.output));
        for (Window& w : windows) {
            if (w.mapped && w.toplevel != nullptr) {
                screen.frame->place(w.toplevel->surface(), w.x, w.y);
            }
        }
        // The cursor last, so it is on top — and only where the hardware isn't
        // already carrying it. A client sprite wins over the theme image; a
        // hidden pointer draws neither. Placements are in layout coordinates,
        // the same space the hit test runs in, and the sprite never answers that
        // hit test: the library marks cursor surfaces input-transparent.
        if (screen.cursor_on_plane || !cursor_placed || seat->cursor_hidden()) {
            return;
        }
        if (luminaria::Surface* sprite = luminaria::surface_from_id(seat->cursor_surface());
            sprite != nullptr) {
            screen.frame->place(*sprite, static_cast<int>(cursor_x) - seat->cursor_hotspot_x(),
                                static_cast<int>(cursor_y) - seat->cursor_hotspot_y());
        } else if (ensure_cursor_texture()) {
            screen.frame->place(*cursor_texture,
                                static_cast<int>(cursor_x) - cursor_texture_image->hotspot_x,
                                static_cast<int>(cursor_y) - cursor_texture_image->hotspot_y,
                                cursor_texture_image->width, cursor_texture_image->height);
        }
    };

    // Everything about the pointer's picture that is NOT its position: which
    // image, and whether each screen's plane can carry it. Called when the
    // picture changes — a client setting or hiding its cursor, a named-shape
    // request, focus moving to another client, the sprite committing a new
    // buffer, a screen appearing — and never on plain motion, which only needs
    // move_cursor().
    //
    // `force` is for the cases the picture changed without any of that being
    // visible from here: the sprite redrew itself, or a screen just appeared.
    struct PushedCursor {
        const char* name = nullptr;
        luminaria::SurfaceId sprite;
        bool hidden = false;
        bool valid = false;
    } pushed;
    auto sync_cursor = [&](bool force = false) {
        const bool hidden = seat->cursor_hidden();
        luminaria::Surface* sprite = luminaria::surface_from_id(seat->cursor_surface());
        // Clients repeat themselves; re-uploading the same picture would spend
        // an atomic commit per request for no change on screen.
        if (!force && pushed.valid && pushed.hidden == hidden &&
            pushed.sprite == seat->cursor_surface() && pushed.name == cursor_name) {
            return;
        }
        pushed = PushedCursor{cursor_name, seat->cursor_surface(), hidden, true};
        int width = 0, height = 0, hotspot_x = 0, hotspot_y = 0;
        std::span<const std::uint8_t> rgba;
        if (!hidden && sprite != nullptr) {
            if (sprite->current_buffer_rgba(cursor_pixels, width, height)) {
                rgba = cursor_pixels;
                hotspot_x = seat->cursor_hotspot_x();
                hotspot_y = seat->cursor_hotspot_y();
            }
        } else if (!hidden) {
            if (const luminaria::CursorImage* image = themed_cursor(); image != nullptr) {
                rgba = image->rgba;
                width = image->width;
                height = image->height;
                hotspot_x = image->hotspot_x;
                hotspot_y = image->hotspot_y;
            }
        }
        for (Screen& sc : screens) {
            const bool was_on_plane = sc.cursor_on_plane;
            sc.cursor_on_plane = false;
            if (sc.output->has_cursor_plane()) {
                if (rgba.empty()) {
                    (void)sc.output->hide_cursor();
                    sc.cursor_on_plane = true; // nothing to draw, hardware agrees
                } else if (sc.output->set_cursor(rgba, width, height, hotspot_x, hotspot_y)) {
                    sc.cursor_on_plane = true;
                } else {
                    // Too big for the plane, or the commit failed: composite it
                    // rather than leave the last picture stuck there.
                    (void)sc.output->hide_cursor();
                }
            }
            // Whatever the cursor was covering has to be repainted when it
            // stops being the hardware's problem, or starts being it.
            if (!sc.cursor_on_plane || was_on_plane != sc.cursor_on_plane) {
                sc.frame->damage_all();
            }
        }
    };
    // A client that redraws its cursor sprite (an animated one, or the first
    // buffer arriving after set_cursor) has to reach the plane too.
    luminaria::Signal<luminaria::SurfaceCommit>::Connection cursor_commit_conn;

    // A window appearing or vanishing changes pixels nobody reported damage for.
    auto damage_everything = [&screens] {
        for (Screen& sc : screens) {
            sc.frame->damage_all();
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

        // Double-buffered GPU scanout: the frame composites into a dmabuf the
        // GPU renders to and KMS scans out. Nothing on this path travels
        // through the CPU. Falling back is the frame's business, not ours.
        if (auto s = screen.frame->reset(kScanoutFormat); !s) {
            std::fprintf(stderr, "luminaria-tty: scanout: %s\n", s.error().message.c_str());
        } else if (screen.frame->target_count() < 2) {
            std::fprintf(stderr, "luminaria-tty: falling back to CPU read-back scanout\n");
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
        std::printf("luminaria-tty: output %dx%d@%.3gHz at (%d,%d), scale %d, cursor plane %s\n",
                    ow, oh, output.current_mode().refresh_mhz / 1000.0, view.x, view.y,
                    output.scale(), output.has_cursor_plane() ? "yes" : "no (composited)");
    };

    auto out_conn = drm->new_output.connect([&](luminaria::NewOutput& e) {
        Screen& screen = screens.emplace_back();
        screen.output = &e.output;
        screen.frame.emplace(e.output, *renderer);
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
        // A monitor that just appeared has an empty cursor plane and a
        // cursor_on_plane nobody has decided yet.
        sync_cursor(/*force=*/true);

        // A frame is on screen: pace the clients that drew it, and answer their
        // presentation feedback with the vblank timestamp the kernel gave us.
        screen.present_conn = e.output.present.connect([&, &screen = screen]
                                                       (luminaria::PresentEvent& pe) {
            // The flip landed, so whatever a direct scanout replaced is off the
            // screen and its client can have it back.
            screen.frame->presented();
            for (Window& w : windows) {
                if (!w.mapped || w.toplevel == nullptr) {
                    continue;
                }
                luminaria::Surface& surface = w.toplevel->surface();
                surface.send_frame_done(pe.time_ms());
                presentation->notify_presented(surface, pe);
            }
        });

        screen.frame_conn = e.output.frame.connect([&, &screen = screen](luminaria::FrameEvent&) {
            constexpr luminaria::Color background{0.1f, 0.1f, 0.13f, 1.0f};
            // Everything below this line — damage bookkeeping against the buffer
            // being drawn into, occlusion, the fences between GPU and display,
            // and the decision to hand a fullscreen client's own buffer straight
            // to the CRTC — is the shell layer's, not this compositor's.
            build_placements(screen);
            auto presented = screen.frame->submit(background);
            if (!presented) {
                std::fprintf(stderr, "luminaria-tty: submit: %s\n",
                             presented.error().message.c_str());
                return;
            }
            frames.count(*presented);
        });
    });

    // --- libinput -> seat -----------------------------------------------------
    //
    // libinput reports relative motion; the compositor owns the cursor position
    // (declared with the rest of the cursor state above). It lives in layout
    // coordinates so it can cross between monitors, and the hit test runs
    // against the same layer list the renderer draws.
    //
    // A client asking for a named shape (wp_cursor_shape_v1) just changes which
    // theme image we show.
    auto cursor_shape_conn =
        cursor_shape->request().connect([&](luminaria::CursorShapeRequest& r) {
            cursor_name = r.name;
            sync_cursor();
        });
    // The focused client setting its own cursor surface, or hiding the pointer.
    auto cursor_change_conn =
        seat->cursor_changed().connect([&](luminaria::SeatCursorChange& e) {
            cursor_commit_conn = {};
            if (luminaria::Surface* sprite = luminaria::surface_from_id(e.surface);
                sprite != nullptr) {
                cursor_commit_conn = sprite->commit.connect(
                    [&](luminaria::SurfaceCommit&) { sync_cursor(/*force=*/true); });
            }
            sync_cursor();
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
    // coordinates — from the placement list of the screen the pointer is on, so
    // the answer is by construction the same one the renderer drew. Rebuilt
    // here rather than reused from the last frame: it costs nothing, and the
    // returned identity remains safe across dispatch.
    auto hit_test = [&](double x, double y, double& lx, double& ly) {
        luminaria::Output* on = layout.at(static_cast<int>(x), static_cast<int>(y));
        if (on == nullptr) {
            return luminaria::SurfaceId{};
        }
        for (Screen& sc : screens) {
            if (sc.output == on) {
                build_placements(sc);
                return sc.frame->surface_at(x, y, lx, ly);
            }
        }
        return luminaria::SurfaceId{};
    };
    auto window_of = [&windows](luminaria::SurfaceId surface) -> Window* {
        for (Window& w : windows) {
            if (!w.mapped || w.toplevel == nullptr) {
                continue;
            }
            for (const luminaria::SurfaceAt& at : w.toplevel->surface().surface_tree()) {
                if (at.surface->id() == surface) {
                    return &w;
                }
            }
        }
        return nullptr;
    };
    luminaria::SurfaceId pointer_focus;
    auto deliver_motion = [&] {
        clamp_cursor();
        luminaria::Output* cursor_now_on = nullptr;
        for (Screen& sc : screens) {
            const luminaria::Box view = layout.box_of(*sc.output);
            const bool here = view.contains(static_cast<int>(cursor_x), static_cast<int>(cursor_y));
            if (here) {
                cursor_now_on = sc.output;
            }
            if (!sc.cursor_on_plane) {
                // Composited: the screen the pointer is on has to be repainted,
                // and so does the one it just left — the cursor it was covering
                // is still drawn there. A whole screen for a 24-pixel sprite is
                // more than it takes; damaging the two cursor rects is what a
                // compositor that cares would do.
                if (here || sc.output == cursor_prev_output) {
                    sc.frame->damage_all();
                }
                continue;
            }
            // Hardware cursor: one small atomic commit, no repaint at all.
            if (here) {
                (void)sc.output->move_cursor(static_cast<int>(cursor_x) - view.x,
                                             static_cast<int>(cursor_y) - view.y);
            } else {
                (void)sc.output->hide_cursor();
            }
        }
        cursor_prev_output = cursor_now_on;
        double lx = 0, ly = 0;
        const luminaria::SurfaceId hit = hit_test(cursor_x, cursor_y, lx, ly);
        luminaria::Surface* surface = luminaria::surface_from_id(hit);
        if (surface == nullptr) {
            if (pointer_focus.valid()) {
                seat->pointer_clear_focus();
                pointer_focus = {};
            }
            return;
        }
        if (hit != pointer_focus) {
            seat->pointer_enter(*surface, lx, ly);
            pointer_focus = hit;
        } else {
            seat->pointer_motion(lx, ly);
        }
    };

    // The backend computes modifier masks against ITS keymap, so the seat has to
    // hand clients the same one — otherwise Shift means one thing here and
    // another in the client, and anything but a US layout types the wrong keys.
    if (!seat->set_keymap(input->keymap())) {
        std::fprintf(stderr, "luminaria-tty: backend keymap rejected, keeping ours\n");
    }
    auto key_conn = input->key().connect([&](luminaria::KeyEvent& e) {
        if (e.pressed && e.keycode == KEY_ESC) {
            display->terminate();
        } else {
            seat->notify_key(e.keycode, e.pressed);
        }
    });
    auto mods_conn = input->modifiers().connect([&](luminaria::ModifiersEvent& e) {
        seat->notify_modifiers(e.depressed, e.latched, e.locked, e.group);
    });
    auto axis_conn = input->pointer_axis().connect([&](luminaria::PointerAxisEvent& e) {
        // Wheel notches win over the smooth deltas that accompany them:
        // forwarding both would scroll twice.
        if (e.dx_steps != 0 || e.dy_steps != 0) {
            seat->pointer_axis_discrete(e.dx_steps, e.dy_steps);
        } else if (e.dx != 0.0 || e.dy != 0.0) {
            seat->pointer_axis(e.dx, e.dy);
        }
        if (e.stop_horizontal || e.stop_vertical) {
            seat->pointer_axis_stop(e.stop_horizontal, e.stop_vertical);
        }
    });
    // What the seat advertises follows what is actually plugged in — a client
    // that binds a wl_pointer no device can drive waits forever. Touch stays off
    // regardless: this compositor routes none, and saying otherwise is a lie
    // clients act on.
    auto caps_conn =
        input->capabilities_changed().connect([&](luminaria::InputCapabilities& caps) {
            seat->set_capabilities(caps.keyboard, caps.pointer, /*touch=*/false);
            std::printf("luminaria-tty: input devices: keyboard=%s pointer=%s touch=%s\n",
                        caps.keyboard ? "yes" : "no", caps.pointer ? "yes" : "no",
                        caps.touch ? "yes (not routed)" : "no");
        });
    auto motion_conn = input->pointer_motion().connect([&](luminaria::PointerMotionEvent& e) {
        cursor_x += e.dx;
        cursor_y += e.dy;
        deliver_motion();
    });
    auto btn_conn = input->pointer_button().connect([&](luminaria::PointerButtonEvent& e) {
        // A press on a window raises keyboard focus with it, the way every
        // click-to-focus desktop behaves.
        if (e.pressed && pointer_focus.valid()) {
            if (Window* w = window_of(pointer_focus); w != nullptr) {
                seat->set_keyboard_focus(&w->toplevel->surface());
            }
        }
        seat->pointer_button(e.button, e.pressed);
    });
    // A surface that dies while the pointer is over it must not stay cached.
    auto pointer_focus_conn =
        seat->pointer_focus_changed().connect([&](luminaria::SeatPointerFocus& e) {
            pointer_focus = e.surface;
            // A named shape belongs to the client that asked for it. Leave its
            // window and the pointer goes back to the plain arrow, otherwise a
            // text field's I-beam follows the pointer across the whole screen.
            cursor_name = "default";
            sync_cursor();
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
