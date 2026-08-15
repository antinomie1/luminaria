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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <drm_fourcc.h>
#include <linux/input-event-codes.h>

import luminaria.gpu;

namespace {
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
    int saved_x = 0, saved_y = 0;
    int saved_width = 0, saved_height = 0;
    int pending_x = 0, pending_y = 0;
    int pending_width = 0, pending_height = 0;
    bool mapped = false;
    bool minimized = false;
    bool geometry_pending = false;
    luminaria::Signal<luminaria::ToplevelMap>::Connection on_map;
    luminaria::Signal<luminaria::ToplevelUnmap>::Connection on_unmap;
    luminaria::Signal<luminaria::ToplevelDestroy>::Connection on_destroy;
    luminaria::Signal<luminaria::SurfaceCommit>::Connection on_commit;
    luminaria::Signal<luminaria::ToplevelRequestMaximize>::Connection on_maximize;
    luminaria::Signal<luminaria::ToplevelRequestFullscreen>::Connection on_fullscreen;
    luminaria::Signal<luminaria::ToplevelRequestMinimize>::Connection on_minimize;
};

struct PopupEntry {
    luminaria::Popup* popup = nullptr;
    bool mapped = false;
    luminaria::Signal<luminaria::PopupMap>::Connection on_map;
    luminaria::Signal<luminaria::PopupUnmap>::Connection on_unmap;
    luminaria::Signal<luminaria::PopupDestroy>::Connection on_destroy;
    luminaria::Signal<luminaria::PopupReposition>::Connection on_reposition;
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
    // Do not advertise xdg-decoration until this example can draw server-side
    // title bars. Qt then uses its built-in client decorations; their requests
    // are handled by the XdgToplevel policy below.

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
    std::list<PopupEntry> popups; // creation order keeps child popups above their parents
    // Set once the screens exist; map/unmap call it to force a full repaint.
    std::function<void()> window_changed = [] {};
    std::function<void(Window&, bool)> maximize_window = [](Window&, bool) {};
    std::function<void(Window&, bool)> fullscreen_window = [](Window&, bool) {};
    std::function<void(Window&)> minimize_window = [](Window&) {};
    auto focus_window = [&](Window* target) {
        for (Window& candidate : windows) {
            if (candidate.toplevel != nullptr) {
                candidate.toplevel->set_activated(&candidate == target);
            }
        }
        seat->set_keyboard_focus(target != nullptr && target->toplevel != nullptr
                                     ? &target->toplevel->surface()
                                     : nullptr);
    };

    auto new_toplevel = shell->new_toplevel().connect([&](luminaria::NewToplevel& e) {
        Window& w = windows.emplace_back();
        w.toplevel = &e.toplevel;
        const int n = static_cast<int>(windows.size()) - 1;
        w.x = w.saved_x = 40 + 30 * n;
        w.y = w.saved_y = 40 + 30 * n;
        w.on_map = e.toplevel.map.connect([&w, &focus_window, &window_changed](luminaria::ToplevelMap&) {
            w.mapped = true;
            w.minimized = false;
            focus_window(&w);
            window_changed();
            std::printf("luminaria-tty: window mapped at (%d,%d), %dx%d — \"%s\"\n", w.x, w.y,
                        w.toplevel->surface().surface_width(),
                        w.toplevel->surface().surface_height(), w.toplevel->title().c_str());
        });
        w.on_unmap = e.toplevel.unmap.connect([&w, &window_changed](luminaria::ToplevelUnmap&) {
            w.mapped = false;
            w.geometry_pending = false;
            window_changed();
        });
        w.on_destroy =
            e.toplevel.destroy.connect([&w, &focus_window, &window_changed](luminaria::ToplevelDestroy&) {
                focus_window(nullptr);
                w.mapped = false;
                w.toplevel = nullptr;
                window_changed();
                std::printf("luminaria-tty: window gone\n");
            });
        w.on_commit = e.toplevel.surface().commit.connect(
            [&w, &window_changed](luminaria::SurfaceCommit&) {
                if (!w.geometry_pending || w.toplevel == nullptr) {
                    return;
                }
                const luminaria::XdgGeometry geometry = w.toplevel->geometry();
                if (geometry.width != w.pending_width || geometry.height != w.pending_height) {
                    return;
                }
                w.x = w.pending_x;
                w.y = w.pending_y;
                w.geometry_pending = false;
                window_changed();
                std::printf("luminaria-tty: window geometry landed at (%d,%d), %dx%d\n", w.x,
                            w.y, geometry.width, geometry.height);
            });
        w.on_maximize = e.toplevel.request_maximize.connect(
            [&w, &maximize_window](luminaria::ToplevelRequestMaximize& request) {
                maximize_window(w, request.maximized);
            });
        w.on_fullscreen = e.toplevel.request_fullscreen.connect(
            [&w, &fullscreen_window](luminaria::ToplevelRequestFullscreen& request) {
                fullscreen_window(w, request.fullscreen);
            });
        w.on_minimize = e.toplevel.request_minimize.connect(
            [&w, &minimize_window](luminaria::ToplevelRequestMinimize&) { minimize_window(w); });
    });

    // Qt menus, combo boxes and tooltips are xdg_popup surfaces, not
    // subsurfaces of the toplevel. Track their lifetime so they join the same
    // placement list used for drawing and hit-testing.
    auto new_popup = shell->new_popup().connect([&](luminaria::NewPopup& e) {
        PopupEntry& p = popups.emplace_back();
        p.popup = &e.popup;
        std::printf("luminaria-tty: popup created at (%d,%d), %dx%d parent=%p\n", e.popup.x(),
                    e.popup.y(), e.popup.width(), e.popup.height(),
                    static_cast<void*>(e.popup.parent_surface()));
        p.on_map = e.popup.map.connect([&p, &window_changed](luminaria::PopupMap&) {
            p.mapped = true;
            window_changed();
            std::printf("luminaria-tty: popup mapped %dx%d at (%d,%d)\n", p.popup->width(),
                        p.popup->height(), p.popup->x(), p.popup->y());
        });
        p.on_unmap = e.popup.unmap.connect([&p, &window_changed](luminaria::PopupUnmap&) {
            p.mapped = false;
            window_changed();
        });
        p.on_destroy = e.popup.destroy.connect([&p, &window_changed](luminaria::PopupDestroy&) {
            p.mapped = false;
            p.popup = nullptr;
            window_changed();
        });
        p.on_reposition =
            e.popup.reposition.connect([&window_changed](luminaria::PopupReposition&) {
                window_changed();
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
    // means a client's own buffer reached the CRTC untouched; `unchanged` means
    // a frame was asked for and turned out to be identical to the one on screen,
    // so nothing was drawn or flipped. An idle screen asks for no frames at all,
    // and this line then simply stops appearing — which is the point.
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
    using SurfaceOrigin = std::pair<luminaria::Surface*, std::pair<int, int>>;
    std::vector<SurfaceOrigin> surface_origins;
    auto build_placements = [&](Screen& screen) {
        screen.frame->begin(layout.box_of(*screen.output));
        surface_origins.clear();
        auto remember_tree_origins = [&](luminaria::Surface& root, int x, int y) {
            for (const luminaria::SurfaceAt& at : root.surface_tree()) {
                surface_origins.push_back({at.surface, {x + at.x, y + at.y}});
            }
        };
        for (Window& w : windows) {
            if (w.mapped && !w.minimized && w.toplevel != nullptr) {
                luminaria::Surface& root = w.toplevel->surface();
                remember_tree_origins(root, w.x, w.y);
                screen.frame->place(root, w.x, w.y);
            }
        }
        for (PopupEntry& p : popups) {
            if (!p.mapped || p.popup == nullptr) {
                continue;
            }
            int px = p.popup->x();
            int py = p.popup->y();
            if (luminaria::Surface* parent_surface = p.popup->parent_surface();
                parent_surface != nullptr) {
                const auto parent = std::find_if(
                    surface_origins.begin(), surface_origins.end(),
                    [parent_surface](const SurfaceOrigin& origin) {
                        return origin.first == parent_surface;
                    });
                if (parent == surface_origins.end()) {
                    std::printf("luminaria-tty: popup skipped: parent not placed\n");
                    continue;
                }
                px += parent->second.first;
                py += parent->second.second;
            }
            luminaria::Surface& root = p.popup->surface();
            remember_tree_origins(root, px, py);
            screen.frame->place(root, px, py);
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

    // Give xdg-positioner the output bounds so a menu near an edge can flip or
    // slide into view instead of being configured off-screen.
    shell->set_popup_constraint_query(
        [&](luminaria::Surface& parent, luminaria::Box& parent_box, luminaria::Box& usable) {
            for (Screen& sc : screens) {
                build_placements(sc);
                for (const luminaria::Placement& placement : sc.frame->placements()) {
                    if (placement.surface != parent.id()) {
                        continue;
                    }
                    // Popup constraints are integer logical boxes, while an
                    // animated placement may sit between pixels. Cover every
                    // touched pixel rather than truncating its right/bottom
                    // edge and constraining a menu against a smaller parent.
                    const int left = static_cast<int>(std::floor(placement.x));
                    const int top = static_cast<int>(std::floor(placement.y));
                    const int right = static_cast<int>(std::ceil(placement.x + placement.width));
                    const int bottom =
                        static_cast<int>(std::ceil(placement.y + placement.height));
                    parent_box = luminaria::Box{left, top, right - left, bottom - top};
                    usable = layout.box_of(*sc.output);
                    return !usable.empty();
                }
            }
            return false;
        });

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
                    auto hidden_status = sc.output->hide_cursor();
                    sc.cursor_on_plane = hidden_status.has_value();
                    if (!hidden_status) {
                        std::fprintf(stderr, "luminaria-tty: hide cursor: %s\n",
                                     hidden_status.error().message.c_str());
                    }
                } else {
                    auto cursor_status =
                        sc.output->set_cursor(rgba, width, height, hotspot_x, hotspot_y);
                    if (cursor_status) {
                        sc.cursor_on_plane = true;
                    } else {
                        // Too big for the plane: hide the old hardware image
                        // before compositing. If hiding itself fails, keep the
                        // old plane state instead of drawing a duplicate cursor.
                        auto hidden_status = sc.output->hide_cursor();
                        if (!hidden_status) {
                            sc.cursor_on_plane = was_on_plane;
                            std::fprintf(stderr, "luminaria-tty: cursor: %s; hide: %s\n",
                                         cursor_status.error().message.c_str(),
                                         hidden_status.error().message.c_str());
                        }
                    }
                }
            }
            // Whatever the cursor was covering has to be repainted when it
            // stops being the hardware's problem, or starts being it. Which
            // rectangles those are is the frame's business: moving on or off
            // the plane adds or removes a placement, and it diffs for that.
            if (!sc.cursor_on_plane || was_on_plane != sc.cursor_on_plane) {
                sc.frame->invalidate();
            }
        }
    };
    // A client that redraws its cursor sprite (an animated one, or the first
    // buffer arriving after set_cursor) has to reach the plane too.
    luminaria::Signal<luminaria::SurfaceCommit>::Connection cursor_commit_conn;

    // A window appearing or vanishing changes pixels nobody reported damage for.
    // All that is owed is the wake-up: the frame finds the rectangles itself by
    // comparing the list it is about to draw against the one it drew.
    auto layout_changed = [&screens] {
        for (Screen& sc : screens) {
            sc.frame->invalidate();
        }
    };
    window_changed = layout_changed;
    auto set_window_covering_layout = [&](Window& window, bool enabled, bool fullscreen) {
        if (window.toplevel == nullptr) {
            return;
        }
        luminaria::Output* output = layout.at(window.x, window.y);
        if (output == nullptr && !screens.empty()) {
            output = screens.front().output;
        }
        const luminaria::Box bounds =
            output != nullptr ? layout.box_of(*output) : luminaria::Box{};
        if (bounds.empty()) {
            return;
        }
        if (enabled) {
            if (!window.toplevel->is_maximized() && !window.toplevel->is_fullscreen()) {
                window.saved_x = window.x;
                window.saved_y = window.y;
                const luminaria::XdgGeometry geometry = window.toplevel->geometry();
                window.saved_width = geometry.width;
                window.saved_height = geometry.height;
            }
            // Keep the old buffer at its old position until the client commits
            // the size requested below. Moving first exposes a one-frame mix of
            // old geometry and new placement, especially on maximize/restore.
            window.pending_x = bounds.x;
            window.pending_y = bounds.y;
            window.pending_width = bounds.width;
            window.pending_height = bounds.height;
            window.geometry_pending = true;
            shell->set_bounds(bounds.width, bounds.height);
        } else {
            window.pending_x = window.saved_x;
            window.pending_y = window.saved_y;
            window.pending_width = window.saved_width;
            window.pending_height = window.saved_height;
            window.geometry_pending = window.saved_width > 0 && window.saved_height > 0;
        }
        if (fullscreen) {
            window.toplevel->set_fullscreen(enabled);
        } else {
            window.toplevel->set_maximized(enabled);
        }
        if (enabled) {
            // The state setter preserves the last configured size. After one
            // restore that size is the normal window geometry, so a second
            // maximize would otherwise send MAXIMIZED + 917x501 (for example),
            // which Qt immediately rejects by requesting restore again. Make
            // the final configure in this batch pair the state with the output.
            (void)window.toplevel->configure(bounds.width, bounds.height);
        } else {
            if (window.geometry_pending) {
                (void)window.toplevel->configure(window.saved_width, window.saved_height);
            } else {
                window.x = window.saved_x;
                window.y = window.saved_y;
                (void)window.toplevel->configure(0, 0);
            }
        }
        std::printf("luminaria-tty: window %s requested — target (%d,%d), %dx%d\n",
                    enabled ? (fullscreen ? "fullscreen" : "maximize") : "restore",
                    window.pending_x, window.pending_y, window.pending_width,
                    window.pending_height);
    };
    maximize_window = [&](Window& window, bool enabled) {
        set_window_covering_layout(window, enabled, false);
    };
    fullscreen_window = [&](Window& window, bool enabled) {
        set_window_covering_layout(window, enabled, true);
    };
    minimize_window = [&](Window& window) {
        if (window.toplevel == nullptr) {
            return;
        }
        window.minimized = true;
        window.geometry_pending = false;
        window.toplevel->set_minimized(true);
        if (seat->keyboard_focus() == window.toplevel->surface().id()) {
            focus_window(nullptr);
        }
        if (seat->pointer_focus() == window.toplevel->surface().id()) {
            seat->pointer_clear_focus();
        }
        layout_changed();
        std::printf("luminaria-tty: window minimized — \"%s\"\n",
                    window.toplevel->title().c_str());
    };
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
        const luminaria::Box all_outputs = layout.bounds();
        shell->set_bounds(all_outputs.width, all_outputs.height);
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
        clamp_cursor();
        sync_cursor(/*force=*/true);

        // A frame is on screen: pace the clients that drew it, and answer their
        // presentation feedback with the vblank timestamp the kernel gave us.
        screen.present_conn = e.output.present.connect([&, &screen = screen]
                                                       (luminaria::PresentEvent& pe) {
            // The flip landed, so whatever a direct scanout replaced is off the
            // screen and its client can have it back.
            screen.frame->presented();
            build_placements(screen);
            for (const luminaria::Placement& placement : screen.frame->placements()) {
                if (luminaria::Surface* surface =
                        luminaria::surface_from_id(placement.surface);
                    surface != nullptr) {
                    surface->send_frame_done(pe.time_ms());
                    presentation->notify_presented(*surface, pe);
                }
            }
        });

        screen.frame_conn = e.output.frame.connect([&, &screen = screen](luminaria::FrameEvent&) {
            constexpr luminaria::Color background{0.1f, 0.1f, 0.13f, 1.0f};
            // Destroy callbacks only null entries: erase them here, after the
            // callback that owns their Connection has returned.
            std::erase_if(popups, [](const PopupEntry& popup) { return popup.popup == nullptr; });
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
            if (*presented == luminaria::Presented::unchanged) {
                // Nothing was flipped, so no vblank is coming and the `present`
                // handler above will not run. The clients still have to be
                // released: one that committed without damaging anything is
                // waiting on a frame callback, and withholding it would freeze
                // it for good. No presentation feedback — nothing was presented.
                timespec now{};
                clock_gettime(CLOCK_MONOTONIC, &now);
                const auto time_ms = static_cast<std::uint32_t>(
                    now.tv_sec * 1000 + now.tv_nsec / 1000000);
                for (const luminaria::Placement& placement : screen.frame->placements()) {
                    if (luminaria::Surface* surface =
                            luminaria::surface_from_id(placement.surface);
                        surface != nullptr) {
                        surface->send_frame_done(time_ms);
                    }
                }
            }
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
            if (!w.mapped || w.minimized || w.toplevel == nullptr) {
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
    auto is_popup_surface = [&popups](luminaria::SurfaceId surface) {
        for (PopupEntry& p : popups) {
            if (!p.mapped || p.popup == nullptr) {
                continue;
            }
            for (const luminaria::SurfaceAt& at : p.popup->surface().surface_tree()) {
                if (at.surface->id() == surface) {
                    return true;
                }
            }
        }
        return false;
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
                // is still drawn there. Both only need waking; the two rects the
                // sprite moved between are what the frame's diff works out, so a
                // 24-pixel cursor costs 24 pixels rather than a screen.
                if (here || sc.output == cursor_prev_output) {
                    sc.frame->invalidate();
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
    bool left_ctrl = false, right_ctrl = false, left_alt = false, right_alt = false;
    auto vt_for_key = [](std::uint32_t key) {
        if (key >= KEY_F1 && key <= KEY_F10) {
            return static_cast<int>(key - KEY_F1 + 1);
        }
        if (key == KEY_F11) {
            return 11;
        }
        if (key == KEY_F12) {
            return 12;
        }
        return 0;
    };
    auto key_conn = input->key().connect([&](luminaria::KeyEvent& e) {
        switch (e.keycode) {
        case KEY_LEFTCTRL: left_ctrl = e.pressed; break;
        case KEY_RIGHTCTRL: right_ctrl = e.pressed; break;
        case KEY_LEFTALT: left_alt = e.pressed; break;
        case KEY_RIGHTALT: right_alt = e.pressed; break;
        default: break;
        }
        const int vt = vt_for_key(e.keycode);
        if (e.pressed && vt != 0 && (left_ctrl || right_ctrl) && (left_alt || right_alt) &&
            seat_session != nullptr) {
            // The shortcut belongs to the compositor. Release modifiers from
            // the focused client before libinput is suspended, otherwise the
            // client may keep Ctrl/Alt logically held after switching back.
            if (left_ctrl) seat->notify_key(KEY_LEFTCTRL, false);
            if (right_ctrl) seat->notify_key(KEY_RIGHTCTRL, false);
            if (left_alt) seat->notify_key(KEY_LEFTALT, false);
            if (right_alt) seat->notify_key(KEY_RIGHTALT, false);
            seat->notify_modifiers(0, 0, 0, 0);
            if (auto switched = seat_session->switch_to(vt); !switched) {
                std::fprintf(stderr, "luminaria-tty: VT switch to %d: %s\n", vt,
                             switched.error().message.c_str());
            } else {
                std::printf("luminaria-tty: requested VT %d\n", vt);
            }
            return;
        }
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
        // An xdg_popup grab owns menu interaction. Clicking anywhere outside
        // its surface dismisses it before the underlying window sees the click.
        if (e.pressed && !is_popup_surface(pointer_focus)) {
            for (PopupEntry& p : popups) {
                if (p.popup != nullptr && p.popup->has_grab()) {
                    p.popup->dismiss();
                }
            }
        }
        // A press on a window raises keyboard focus with it, the way every
        // click-to-focus desktop behaves.
        if (e.pressed && pointer_focus.valid()) {
            if (Window* w = window_of(pointer_focus); w != nullptr) {
                focus_window(w);
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
