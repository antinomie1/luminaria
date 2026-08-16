// tinyluminaria — a minimal reference compositor built on luminaria. Mirrors tinywl: wire
// up the backend, compositor, xdg-shell, seat, and scene, then run the loop.
//
// Runs on the headless backend (no GPU/display needed to smoke-test the wiring).
// Env knobs: LUMINARIA_BACKEND=headless forces headless, LUMINARIA_OUTPUT=WxH
// sets the output size, LUMINARIA_EXIT_MS auto-terminates after N ms (smoke test).
#include "detail/wayland_fwd.h"

#include <cstdint>
#include <stdlib.h>
#include <cstdio>
#include <ctime>
#include <drm_fourcc.h>

import luminaria.gpu;
import luminaria.desktop;
import std;

namespace {
template <class T>
T must(luminaria::Result<T> r, const char* what) {
    if (!r) {
        std::fprintf(stderr, "tinyluminaria: %s: %s\n", what, r.error().message.c_str());
        std::exit(1);
    }
    return std::move(*r);
}

constexpr std::uint32_t kScanoutFormat = DRM_FORMAT_XRGB8888;

/// Output size, overridable with LUMINARIA_OUTPUT=WxH. Anything unparseable
/// falls back to the default rather than producing a degenerate output.
void output_size(int& width, int& height) {
    width = 800;
    height = 600;
    const char* spec = std::getenv("LUMINARIA_OUTPUT");
    if (spec == nullptr) {
        return;
    }
    int w = 0, h = 0;
    if (std::sscanf(spec, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
        width = w;
        height = h;
    } else {
        std::fprintf(stderr, "tinyluminaria: ignoring LUMINARIA_OUTPUT=%s (want WxH)\n", spec);
    }
}

struct Window {
    luminaria::Toplevel* toplevel = nullptr;
    int x = 0, y = 0;
    int saved_x = 0, saved_y = 0; // geometry to restore when unmaximizing
    bool mapped = false;
    luminaria::Signal<luminaria::ToplevelMap>::Connection on_map;
    luminaria::Signal<luminaria::ToplevelUnmap>::Connection on_unmap;
    luminaria::Signal<luminaria::ToplevelDestroy>::Connection on_destroy;
    luminaria::Signal<luminaria::ToplevelRequestMaximize>::Connection on_maximize;
    luminaria::Signal<luminaria::ToplevelRequestFullscreen>::Connection on_fullscreen;
};

struct PopupEntry {
    luminaria::Popup* popup = nullptr;
    bool mapped = false;
    luminaria::Signal<luminaria::PopupMap>::Connection on_map;
    luminaria::Signal<luminaria::PopupUnmap>::Connection on_unmap;
    luminaria::Signal<luminaria::PopupDestroy>::Connection on_destroy;
};

/// A built-in arrow, used whenever the focused client hasn't set a cursor of
/// its own (wl_pointer.set_cursor). RGBA, hotspot at (0,0).
constexpr int kCursorW = 10;
constexpr int kCursorH = 16;
std::vector<std::uint8_t> make_default_cursor() {
    std::vector<std::uint8_t> rgba(static_cast<size_t>(kCursorW) * kCursorH * 4, 0);
    for (int y = 0; y < kCursorH; ++y) {
        // A triangle that narrows toward the bottom, with a 1px black outline.
        const int span = std::min(kCursorW, kCursorH - y);
        for (int x = 0; x < span; ++x) {
            const bool edge = x == 0 || x == span - 1 || y == 0;
            std::uint8_t* px = rgba.data() + (static_cast<size_t>(y) * kCursorW + x) * 4;
            px[0] = px[1] = px[2] = edge ? 0 : 255;
            px[3] = 255;
        }
    }
    return rgba;
}
} // namespace

int main() {
    int output_width = 0, output_height = 0;
    output_size(output_width, output_height);

    // Optional GPU compositor. Without it (no Vulkan device) we fall back to a
    // solid background — the window still runs, just isn't drawn.
    //
    // Declared before the Display on purpose: surfaces cache GPU textures that
    // belong to this renderer, and locals are destroyed in reverse order.
    std::unique_ptr<luminaria::VulkanRenderer> renderer;
    if (auto r = luminaria::VulkanRenderer::create()) {
        renderer = std::make_unique<luminaria::VulkanRenderer>(std::move(*r));
        std::printf("tinyluminaria: compositing = GPU (Vulkan)\n");
    } else {
        std::printf("tinyluminaria: compositing = DISABLED, background only (no vulkan: %s)\n",
                    r.error().message.c_str());
    }

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
            nested->add_output(output_width, output_height, "tinyluminaria");
            backend = nested.get();
            std::printf("tinyluminaria: nested backend (window in parent compositor)\n");
        }
    }
    if (backend == nullptr) {
        headless = std::make_unique<luminaria::HeadlessBackend>(display.event_loop());
        headless->add_output(output_width, output_height);
        backend = headless.get();
        std::printf("tinyluminaria: headless backend\n");
    }

    auto compositor = must(luminaria::Compositor::create(display), "compositor");
    auto subcompositor = must(luminaria::Subcompositor::create(display), "wl_subcompositor");
    auto shell = must(luminaria::XdgShell::create(display), "xdg-shell");
    auto seat = must(luminaria::Seat::create(display), "seat");
    // A 1x1 solid-colour wl_buffer, so clients don't allocate a full-screen one
    // just to paint a backdrop.
    auto single_pixel =
        must(luminaria::SinglePixelBufferManager::create(display), "single-pixel-buffer");
    // Frame timing: clients that animate need to know when a frame landed and
    // how long a refresh is. Fed from Output::present below.
    auto presentation = must(luminaria::Presentation::create(display), "presentation-time");
    // Games asking to skip the vblank wait; forwarded to the output that owns
    // the fullscreen surface (only the DRM backend can actually tear).
    auto tearing = must(luminaria::TearingControlManager::create(display), "tearing-control");
    // The other half of pacing, and the reason an animating client can sleep:
    // FIFO parks a commit until the previous frame has been shown (the barrier
    // is cleared by send_frame_done() in the present handler below), and commit
    // timing parks one until a named instant. Neither needs anything else here.
    auto fifo = must(luminaria::FifoManager::create(display), "wp-fifo");
    auto commit_timing =
        must(luminaria::CommitTimingManager::create(display), "wp-commit-timing");
    // "I am playing a video" / "I am a game", for whoever decides refresh rate
    // and scanout policy. Read it off the Surface.
    auto content_type = must(luminaria::ContentTypeManager::create(display), "content-type");
    // Client-declared background blur regions. Frame exposes the committed
    // Surface::blur_region(); this tiny compositor keeps its simple flat
    // background, while a visual shell can feed those regions to x-ray blur.
    auto background_effect =
        must(luminaria::BackgroundEffectManager::create(display), "background-effect");
    // Named cursors, so clients stop shipping their own bitmaps.
    auto cursor_shape = must(luminaria::CursorShapeManager::create(display), "cursor-shape");
    // Crop/stretch a buffer, and tell clients the true (fractional) output
    // scale. The two go together: a client renders at 1.5x into an integer
    // buffer, then uses a viewport to declare the logical size it stands for.
    auto viewporter = must(luminaria::Viewporter::create(display), "wp_viewporter");
    auto fractional = must(luminaria::FractionalScaleManager::create(display), "fractional-scale");
    // Who draws the title bar. We draw nothing, so clients are told to decorate
    // themselves — a window with no frame at all is worse than a toolkit one.
    auto decoration = must(luminaria::XdgDecorationManager::create(display), "xdg-decoration");
    decoration.set_default_mode(luminaria::DecorationMode::client_side);
    // Tell clients how big a window we recommend (xdg_toplevel.configure_bounds).
    shell.set_bounds(output_width, output_height);
    // Clipboard, drag-and-drop, and middle-click paste. Both follow seat focus.
    auto data_device = must(luminaria::DataDeviceManager::create(display, seat), "data-device");
    auto primary_selection =
        must(luminaria::PrimarySelectionManager::create(display, seat), "primary-selection");
    // Real clients (weston-terminal) won't map a window until they see an output.
    auto output_global =
        must(luminaria::OutputGlobal::create(display, output_width, output_height), "wl_output");

    // Workspaces, so a panel/pager can list and switch them. Purely declarative:
    // the compositor owns the set, clients only ask.
    auto workspaces = must(luminaria::WorkspaceManager::create(display), "ext-workspace");
    const std::uint32_t ws_group = workspaces.add_group();
    workspaces.group_add_output(ws_group, output_global);
    std::vector<std::uint32_t> ws_ids;
    for (int i = 1; i <= 2; ++i) {
        ws_ids.push_back(workspaces.add_workspace(ws_group, "ws-" + std::to_string(i),
                                                  std::to_string(i), {i - 1}));
    }
    workspaces.activate(ws_ids.front());
    workspaces.done();
    auto ws_request = workspaces.request().connect([&](luminaria::WorkspaceRequest& r) {
        // Switching is the compositor's call; here it just moves the active flag.
        if (r.kind == luminaria::WorkspaceRequest::Kind::activate) {
            workspaces.activate(r.workspace);
            workspaces.done();
        }
    });

    // A named-cursor request replaces the client's cursor surface; we have no
    // theme loader, so record the name and keep drawing the built-in arrow.
    // (The connection itself is made below, once there is a frame to repaint.)
    const char* cursor_name = "default";
    luminaria::Signal<luminaria::CursorShapeRequest>::Connection cursor_shape_conn;
    luminaria::Signal<luminaria::SeatCursorChange>::Connection cursor_changed_conn;

    // Screencopy: allow tools like grim/slurp to capture the output.
    auto screencopy = must(luminaria::ScreencopyManager::create(display), "screencopy");

    // Per-output state — each output owns its own frame cache, GPU scanout
    // target, and connections. Nothing is shared across outputs, so a hotplug
    // event can never corrupt another monitor's data.
    struct PerOutput {
        std::optional<luminaria::Frame> frame; // the shell layer's ledger
        luminaria::Signal<luminaria::OutputDestroy>::Connection on_destroy;
        luminaria::Signal<luminaria::FrameEvent>::Connection on_frame;
        luminaria::Signal<luminaria::PresentEvent>::Connection on_present;
    };
    std::map<luminaria::Output*, PerOutput> per_output;

    // "Something changed that no client reported": a window appearing or
    // vanishing, a popup, the pointer moving over a composited cursor. None of
    // those produce surface damage, and none of them would otherwise reach the
    // screen — outputs only deliver a frame when someone asks, and this is the
    // asking. A client redrawing its own window needs nothing here; the frame
    // is already watching every surface it draws.
    //
    // Only the asking: what actually changed is worked out by the frame, which
    // compares the list it is about to draw against the one it drew last. So
    // moving a window costs the two rectangles it moved between, not a screen.
    auto layout_changed = [&per_output] {
        for (auto& [output, po] : per_output) {
            if (po.frame.has_value()) {
                po.frame->invalidate();
            }
        }
    };

    // The pointer's picture is drawn by us, so every change to it is one of
    // those unreported changes — the shape a client asked for by name, or the
    // client swapping in a cursor surface of its own, which the frame is not
    // watching until it has drawn it once.
    cursor_shape_conn = cursor_shape.request().connect(
        [&cursor_name, &layout_changed](luminaria::CursorShapeRequest& r) {
            cursor_name = r.name;
            layout_changed();
        });
    cursor_changed_conn = seat.cursor_changed().connect(
        [&layout_changed](luminaria::SeatCursorChange&) { layout_changed(); });

    output_global.on_bind([&](wl_resource* res) {
        screencopy.add_output(res, output_global.width(), output_global.height(),
            [&per_output, ow = output_global.width()]
            (int x, int y, int w, int h, std::vector<uint8_t>& rgba) -> bool {
                if (per_output.empty() || !per_output.begin()->second.frame.has_value()) {
                    return false;
                }
                // Single-output compositor: use the first (only) output's frame.
                // On the zero-copy path nothing is read back per frame, so the
                // read happens here — only when something actually asks for a
                // capture.
                auto shown = per_output.begin()->second.frame->read_back();
                if (!shown || shown->empty()) {
                    return false;
                }
                // Extract subregion from the full frame.
                rgba.resize(static_cast<size_t>(w) * h * 4);
                for (int row = 0; row < h; ++row) {
                    const auto* src = shown->data() + (y + row) * ow + x;
                    auto* dst = reinterpret_cast<uint8_t*>(rgba.data()) + row * w * 4;
                    std::memcpy(dst, src, static_cast<size_t>(w) * 4);
                }
                return true;
            });
    });

    // With a GPU: advertise linux-dmabuf (GPU clients) and let screencopy write
    // dmabuf capture targets. Both no-op gracefully if there's no render node.
    std::optional<luminaria::LinuxDmabuf> dmabuf;
    if (renderer) {
        if (auto d = luminaria::LinuxDmabuf::create(display, renderer.get())) {
            dmabuf.emplace(std::move(*d));
            std::printf("tinyluminaria: linux-dmabuf = enabled\n");
        } else {
            std::printf("tinyluminaria: linux-dmabuf = off (%s)\n", d.error().message.c_str());
        }
        screencopy.set_renderer(renderer.get());
    }

    // Explicit GPU synchronisation for clients that don't want implicit fences.
    std::optional<luminaria::DrmSyncobjManager> syncobj;
    if (auto s = luminaria::DrmSyncobjManager::create(display)) {
        syncobj.emplace(std::move(*s));
        std::printf("tinyluminaria: linux-drm-syncobj = enabled\n");
    } else {
        std::printf("tinyluminaria: linux-drm-syncobj = off (%s)\n", s.error().message.c_str());
    }

    std::list<Window> windows;    // stable addresses for the process lifetime
    std::list<PopupEntry> popups; // creation order: a child popup follows its parent
    luminaria::SurfaceId ptr_focus; // surface under the cursor
    auto ptr_focus_conn =
        seat.pointer_focus_changed().connect([&](luminaria::SeatPointerFocus& event) {
            ptr_focus = event.surface;
        });
    int ptr_x = 0, ptr_y = 0; // cursor position, output coordinates
    bool ptr_inside = false;
    // Real cursors from the user's theme, so a client asking for "text" gets an
    // I-beam rather than the same arrow as everything else. The hand-drawn
    // arrow stays as the fallback for a machine with no themes installed.
    const std::vector<std::uint8_t> builtin_cursor = make_default_cursor();
    auto cursor_theme = luminaria::CursorTheme::load();
    if (cursor_theme) {
        std::printf("tinyluminaria: cursor theme = %s @ %dpx\n", cursor_theme->name().c_str(),
                    cursor_theme->size());
    } else {
        std::printf("tinyluminaria: cursor theme = built-in arrow (%s)\n",
                    cursor_theme.error().message.c_str());
    }
    // The image for the currently requested shape, or null to use the built-in.
    auto themed_cursor = [&cursor_theme, &cursor_name]() -> const luminaria::CursorImage* {
        return cursor_theme ? cursor_theme->frame(cursor_name, 0) : nullptr;
    };

    // The same image on the GPU. Uploaded when the shape changes, not per frame
    // — the pointer moves constantly but its picture almost never does.
    std::optional<luminaria::GpuTexture> cursor_texture;
    const char* cursor_texture_name = nullptr;
    const luminaria::CursorImage* cursor_texture_image = nullptr;
    auto ensure_cursor_texture = [&]() -> bool {
        const luminaria::CursorImage* image = themed_cursor();
        if (image == nullptr || !renderer) {
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
    // The cursor as a capture source of its own: a screen recorder asking for a
    // cursor session gets these pixels, so it can leave the pointer out of the
    // video and draw it back at its own frame rate. A client-set cursor surface
    // wins over the theme, same as when we composite it.
    auto cursor_capture = [&](wl_resource*, luminaria::CursorCapture& out) {
        if (!ptr_inside || seat.cursor_hidden()) {
            return false;
        }
        if (luminaria::Surface* c = luminaria::surface_from_id(seat.cursor_surface()); c != nullptr) {
            int w = 0, h = 0;
            if (!c->current_buffer_rgba(out.rgba, w, h)) {
                return false;
            }
            out.width = w;
            out.height = h;
            out.hotspot_x = seat.cursor_hotspot_x();
            out.hotspot_y = seat.cursor_hotspot_y();
        } else if (const luminaria::CursorImage* image = themed_cursor(); image != nullptr) {
            out.rgba = image->rgba;
            out.width = image->width;
            out.height = image->height;
            out.hotspot_x = image->hotspot_x;
            out.hotspot_y = image->hotspot_y;
        } else {
            return false;
        }
        out.x = ptr_x;
        out.y = ptr_y;
        return true;
    };
    screencopy.set_cursor_source(cursor_capture);

    // Where the outputs sit relative to each other. One output here, but the
    // layout is what xdg-output and multi-monitor hit-testing quote.
    luminaria::OutputLayout layout;
    const luminaria::Color kBg{0.1f, 0.1f, 0.12f, 1.0f};

    // Per-output frame ledgers, created once the outputs exist. `Frame` is the
    // shell layer: it holds the scanout buffers, the damage each of them still
    // owes, and the decision of how this frame reaches the screen.
    std::map<luminaria::Output*, luminaria::Frame*> frames;

    // Refill one frame's placement list, back-to-front: toplevels with their
    // subsurface trees, then popups anchored to their parents, then the cursor
    // on top. This is the list that is both drawn and hit-tested, so the two
    // cannot disagree; it is rebuilt whenever either is needed, and rebuilding
    // it allocates nothing.
    auto build_placements = [&](luminaria::Frame& frame, luminaria::Output& output) {
        frame.begin(layout.box_of(output));
        std::map<luminaria::Surface*, std::pair<int, int>> origin;
        for (Window& w : windows) {
            if (!w.mapped || w.toplevel == nullptr) {
                continue;
            }
            luminaria::Surface& root = w.toplevel->surface();
            origin[&root] = {w.x, w.y};
            frame.place(root, w.x, w.y);
        }
        for (PopupEntry& p : popups) {
            if (!p.mapped || p.popup == nullptr) {
                continue;
            }
            auto parent = origin.find(p.popup->parent_surface());
            if (parent == origin.end()) {
                continue; // parent isn't on screen; neither is the popup
            }
            const int px = parent->second.first + p.popup->x();
            const int py = parent->second.second + p.popup->y();
            luminaria::Surface& root = p.popup->surface();
            origin[&root] = {px, py};
            frame.place(root, px, py);
        }
        // The cursor, composited in: there is no cursor plane on a nested or
        // headless output, so it is a placement like any other. A client sprite
        // wins over the theme image, and a client that asked for no pointer at
        // all gets none — the theme arrow is for "the client never said".
        if (!ptr_inside || seat.cursor_hidden()) {
            return;
        }
        if (luminaria::Surface* cursor = luminaria::surface_from_id(seat.cursor_surface());
            cursor != nullptr) {
            frame.place(*cursor, ptr_x - seat.cursor_hotspot_x(),
                        ptr_y - seat.cursor_hotspot_y());
        } else if (ensure_cursor_texture()) {
            frame.place(*cursor_texture, ptr_x - cursor_texture_image->hotspot_x,
                        ptr_y - cursor_texture_image->hotspot_y, cursor_texture_image->width,
                        cursor_texture_image->height);
        }
    };

    // The frame for a point in the layout, with its placement list already
    // rebuilt — null before any output has appeared, or without a GPU.
    auto frame_at = [&](double x, double y) -> luminaria::Frame* {
        luminaria::Output* on = layout.at(static_cast<int>(x), static_cast<int>(y));
        if (on == nullptr) {
            on = frames.empty() ? nullptr : frames.begin()->first;
        }
        const auto it = on == nullptr ? frames.end() : frames.find(on);
        if (it == frames.end()) {
            return nullptr;
        }
        build_placements(*it->second, *it->first);
        return it->second;
    };

    // Teach the shell where windows are, so xdg_positioner's constraint
    // adjustment can do its job: a menu near the bottom of the screen flips up
    // instead of hanging off the edge. Without this the shell has no idea where
    // on screen anything is and menus simply overhang.
    shell.set_popup_constraint_query([&](luminaria::Surface& parent, luminaria::Box& parent_box,
                                         luminaria::Box& usable) {
        for (const auto& [output, frame] : frames) {
            build_placements(*frame, *output);
            for (const luminaria::Placement& p : frame->placements()) {
                if (p.surface != parent.id()) {
                    continue;
                }
                const int left = static_cast<int>(std::floor(p.x));
                const int top = static_cast<int>(std::floor(p.y));
                const int right = static_cast<int>(std::ceil(p.x + p.width));
                const int bottom = static_cast<int>(std::ceil(p.y + p.height));
                parent_box = luminaria::Box{left, top, right - left, bottom - top};
                usable = layout.box_of(*output);
                return !usable.empty();
            }
        }
        return false;
    });

    auto new_output = backend->new_output.connect([&](luminaria::NewOutput& e) {
        layout.add_auto(e.output);
        const luminaria::Box placed = layout.box_of(e.output);
        output_global.set_logical_position(placed.x, placed.y);

        // Per-output state: each monitor owns its own Frame, and the Frame
        // owns the buffers. They are allocated here rather than per frame —
        // allocating a scanout target every frame would leak GPU memory.
        auto& po = per_output[&e.output];
        if (renderer) {
            po.frame.emplace(e.output, *renderer);
            if (auto s = po.frame->reset(kScanoutFormat); !s) {
                std::fprintf(stderr, "tinyluminaria: scanout target: %s\n",
                             s.error().message.c_str());
                po.frame.reset();
            } else {
                frames[&e.output] = &*po.frame;
                std::printf("tinyluminaria: present = %s\n",
                            po.frame->target_count() < 2 ? "GPU composite + CPU read-back"
                                                         : "direct dmabuf scanout (zero copy)");
            }
        }
        // Output unplugged: drop it from the layout and free its state.
        po.on_destroy = e.output.destroy.connect([&](luminaria::OutputDestroy& ev) {
            layout.remove(ev.output);
            frames.erase(&ev.output);
            per_output.erase(&ev.output);
        });

        // A frame reached the screen: only now do the clients that drew it get
        // told to draw again, and only now is their presentation feedback true.
        po.on_present = e.output.present.connect([&, &po = po](luminaria::PresentEvent& pe) {
            if (!po.frame.has_value()) {
                return;
            }
            build_placements(*po.frame, pe.output);
            for (const luminaria::Placement& p : po.frame->placements()) {
                if (luminaria::Surface* surface = luminaria::surface_from_id(p.surface);
                    surface != nullptr) {
                    surface->send_frame_done(pe.time_ms());
                    presentation.notify_presented(*surface, pe);
                }
            }
        });

        po.on_frame = e.output.frame.connect([&, &po = po](luminaria::FrameEvent& fe) {
            // Reap closed windows and popups here (safe point) — not in the
            // destroy callback, which would free the entry while its own slot
            // is running.
            std::erase_if(windows, [](const Window& w) { return w.toplevel == nullptr; });
            std::erase_if(popups, [](const PopupEntry& p) { return p.popup == nullptr; });

            static int frame_n = 0;
            // Every sixtieth frame — which used to be once a second and is now
            // once every sixty frames anyone actually asked for.
            if (++frame_n % 60 == 1) {
                std::printf("tinyluminaria: frame %d — %zu window(s), %zu popup(s), "
                            "cursor '%s'\n",
                            frame_n, windows.size(), popups.size(), cursor_name);
            }

            // Compositing, damage, fences and the flip are all the shell
            // layer's. This example only says what goes where — and that the
            // cursor moving needs a repaint, since nothing reports damage for
            // a pointer that is not on a plane of its own.
            if (po.frame.has_value()) {
                build_placements(*po.frame, fe.output);
                if (auto presented = po.frame->submit(kBg)) {
                    if (*presented == luminaria::Presented::unchanged) {
                        // Nothing was committed, so no `present` follows and the
                        // clients would wait forever for the frame callbacks that
                        // handler sends. Release them here instead; there is no
                        // presentation feedback to give, nothing was presented.
                        timespec now{};
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        const auto time_ms = static_cast<std::uint32_t>(
                            now.tv_sec * 1000 + now.tv_nsec / 1000000);
                        for (const luminaria::Placement& p : po.frame->placements()) {
                            if (luminaria::Surface* surface =
                                    luminaria::surface_from_id(p.surface);
                                surface != nullptr) {
                                surface->send_frame_done(time_ms);
                            }
                        }
                    }
                    return;
                } else {
                    std::fprintf(stderr, "tinyluminaria: submit: %s\n",
                                 presented.error().message.c_str());
                }
            }
            // No GPU at all (or the frame could not be put on screen): the
            // output still works, it just isn't drawn. Nothing tracks damage on
            // this path, so keep the pump turning by hand — it is a bring-up
            // path, and a stalled one looks like a hang.
            (void)fe.output.commit(kBg);
            fe.output.schedule_frame();
        });
    });

    // Give keyboard focus to a window and tell it so (xdg_toplevel ACTIVATED).
    luminaria::Toplevel* focused = nullptr;
    auto focus_window = [&](Window* w) {
        luminaria::Toplevel* next = w != nullptr ? w->toplevel : nullptr;
        if (focused == next) {
            return;
        }
        if (focused != nullptr) {
            focused->set_activated(false);
        }
        focused = next;
        if (focused != nullptr) {
            focused->set_activated(true);
            seat.set_keyboard_focus(&focused->surface());
        } else {
            seat.set_keyboard_focus(nullptr);
        }
    };

    // Track each new window; focus and mark it mapped on map.
    auto new_toplevel = shell.new_toplevel().connect([&](luminaria::NewToplevel& e) {
        Window& w = windows.emplace_back();
        w.toplevel = &e.toplevel;
        const int n = static_cast<int>(windows.size()) - 1;
        w.x = w.saved_x = 40 + 30 * n;
        w.y = w.saved_y = 40 + 30 * n;
        w.on_map = e.toplevel.map.connect(
            [&w, &focus_window, &fractional, &output_global,
             &layout_changed](luminaria::ToplevelMap&) {
                w.mapped = true;
                layout_changed();
                focus_window(&w);
                // Tell the client what density to render at, both ways: the
                // integer hint every client understands, and the exact scale
                // for the ones that speak fractional-scale.
                luminaria::Surface& surface = w.toplevel->surface();
                surface.set_preferred_buffer_scale(output_global.scale());
                surface.set_preferred_buffer_transform(output_global.transform());
                fractional.set_scale(surface, output_global.scale() * 120);
            });
        w.on_unmap = e.toplevel.unmap.connect([&w, &layout_changed](luminaria::ToplevelUnmap&) {
            w.mapped = false;
            layout_changed();
        });
        w.on_destroy = e.toplevel.destroy.connect(
            [&w, &focused, &focus_window, &layout_changed](luminaria::ToplevelDestroy&) {
                if (focused == w.toplevel) {
                    focus_window(nullptr);
                }
                w.mapped = false;
                w.toplevel = nullptr;
                layout_changed();
            });
        // Window state: we grant maximize/fullscreen and tell the client what
        // size to take. Both cover the whole output here — there's no panel.
        w.on_maximize = e.toplevel.request_maximize.connect(
            [&w](luminaria::ToplevelRequestMaximize& ev) {
                if (ev.maximized) {
                    w.saved_x = w.x;
                    w.saved_y = w.y;
                    w.x = w.y = 0;
                    // set_maximized configures at the shell bounds, which we set
                    // to the output size — no second configure needed.
                    w.toplevel->set_maximized(true);
                } else {
                    w.x = w.saved_x;
                    w.y = w.saved_y;
                    w.toplevel->set_maximized(false);
                    (void)w.toplevel->configure(0, 0); // "pick your own size"
                }
            });
        w.on_fullscreen = e.toplevel.request_fullscreen.connect(
            [&w](luminaria::ToplevelRequestFullscreen& ev) {
                if (ev.fullscreen) {
                    w.saved_x = w.x;
                    w.saved_y = w.y;
                    w.x = w.y = 0;
                    w.toplevel->set_fullscreen(true); // configures at the shell bounds
                } else {
                    w.x = w.saved_x;
                    w.y = w.saved_y;
                    w.toplevel->set_fullscreen(false);
                    (void)w.toplevel->configure(0, 0);
                }
            });
    });

    // Popups (menus, tooltips, combo drop-downs) are positioned by the shell
    // relative to their parent; we only have to track and draw them.
    auto new_popup = shell.new_popup().connect([&](luminaria::NewPopup& e) {
        PopupEntry& p = popups.emplace_back();
        p.popup = &e.popup;
        p.on_map = e.popup.map.connect([&p, &layout_changed](luminaria::PopupMap&) {
            p.mapped = true;
            layout_changed();
        });
        p.on_unmap = e.popup.unmap.connect([&p, &layout_changed](luminaria::PopupUnmap&) {
            p.mapped = false;
            layout_changed();
        });
        p.on_destroy = e.popup.destroy.connect([&p, &layout_changed](luminaria::PopupDestroy&) {
            p.mapped = false;
            p.popup = nullptr;
            layout_changed();
        });
    });

    // Route parent (KDE) input into our seat when running nested. Hit-test the
    // cursor against the same layer list we render, so clicks land where the
    // pixels are — including on subsurfaces and popups.
    luminaria::Signal<luminaria::PointerMotionAbsEvent>::Connection on_ptr_motion;
    luminaria::Signal<luminaria::PointerButtonEvent>::Connection on_ptr_button;
    luminaria::Signal<luminaria::PointerAxisEvent>::Connection on_ptr_axis;
    luminaria::Signal<luminaria::KeyEvent>::Connection on_key;
    luminaria::Signal<luminaria::ModifiersEvent>::Connection on_mods;
    luminaria::Signal<luminaria::KeymapChange>::Connection on_keymap;
    if (nested) {
        // Topmost surface under (x,y), plus the point in its local coordinates.
        // Against the frame's own placement list, so the answer agrees with the
        // pixels by construction. accepts_input honours the client's input
        // region, so a click in a rounded corner or a shadow falls through to
        // whatever is behind.
        auto hit_test = [&](double x, double y, double& lx, double& ly) {
            luminaria::Frame* frame = frame_at(x, y);
            return frame == nullptr ? luminaria::SurfaceId{} : frame->surface_at(x, y, lx, ly);
        };
        // The window (if any) a surface belongs to, so a click can raise+focus it.
        auto window_of = [&](luminaria::SurfaceId surface) -> Window* {
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
        auto is_popup_surface = [&](luminaria::SurfaceId surface) {
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

        on_ptr_motion = nested->pointer_motion.connect([&](luminaria::PointerMotionAbsEvent& e) {
            // The cursor is composited here (a nested output has no cursor
            // plane), and a placed texture reports no damage of its own — so
            // the pointer moving at all is a repaint this compositor has to
            // ask for. A whole output for a 24-pixel sprite is more than it
            // takes; damaging the two cursor rects is what a compositor that
            // cares would do.
            layout_changed();
            if (e.x < 0) { // pointer left our window
                ptr_inside = false;
                ptr_focus = {};
                seat.pointer_clear_focus();
                return;
            }
            ptr_inside = true;
            ptr_x = static_cast<int>(e.x);
            ptr_y = static_cast<int>(e.y);
            double lx = 0, ly = 0;
            const luminaria::SurfaceId hit = hit_test(e.x, e.y, lx, ly);
            luminaria::Surface* surface = luminaria::surface_from_id(hit);
            if (surface == nullptr) {
                ptr_focus = {};
                seat.pointer_clear_focus();
                return;
            }
            if (hit != ptr_focus) {
                ptr_focus = hit;
                seat.pointer_enter(*surface, lx, ly);
            } else {
                seat.pointer_motion(lx, ly);
            }
        });
        on_ptr_button = nested->pointer_button.connect([&](luminaria::PointerButtonEvent& e) {
            // A press outside a grabbing popup dismisses it (menu semantics).
            if (e.pressed && !is_popup_surface(ptr_focus)) {
                for (PopupEntry& p : popups) {
                    if (p.popup != nullptr && p.popup->has_grab()) {
                        p.popup->dismiss();
                    }
                }
            }
            if (!ptr_focus.valid()) {
                return;
            }
            if (e.pressed) {
                if (Window* w = window_of(ptr_focus); w != nullptr) {
                    focus_window(w);
                }
            }
            seat.pointer_button(e.button, e.pressed);
        });
        on_ptr_axis = nested->pointer_axis.connect([&](luminaria::PointerAxisEvent& e) {
            // Wheel notches win over the smooth deltas that accompany them:
            // forwarding both would scroll twice.
            if (e.dx_steps != 0 || e.dy_steps != 0) {
                seat.pointer_axis_discrete(e.dx_steps, e.dy_steps);
            } else if (e.dx != 0.0 || e.dy != 0.0) {
                seat.pointer_axis(e.dx, e.dy);
            }
            if (e.stop_horizontal || e.stop_vertical) {
                seat.pointer_axis_stop(e.stop_horizontal, e.stop_vertical);
            }
        });
        on_key = nested->key.connect(
            [&](luminaria::KeyEvent& e) { seat.notify_key(e.keycode, e.pressed); });
        on_mods = nested->modifiers.connect([&](luminaria::ModifiersEvent& e) {
            seat.notify_modifiers(e.depressed, e.latched, e.locked, e.group);
        });
        // Adopt the parent's keyboard layout. The modifier masks above are
        // computed against it, so anything else agrees only by luck on a US
        // layout — and our clients would see a different layout from the one
        // the user is typing on.
        on_keymap = nested->keymap_changed.connect([&](luminaria::KeymapChange& e) {
            if (!seat.set_keymap(e.text)) {
                std::fprintf(stderr, "tinyluminaria: parent keymap rejected, keeping ours\n");
            }
        });
        if (!nested->keymap().empty()) {
            (void)seat.set_keymap(nested->keymap()); // already arrived during start()
        }
    }

    if (auto socket = display.add_socket_auto()) {
        setenv("WAYLAND_DISPLAY", socket->c_str(), 1);
        std::printf("tinyluminaria running on WAYLAND_DISPLAY=%s\n", socket->c_str());
    }

    (void)backend->start();

    if (nested) {
        // Decoration is negotiated during start(); report what the host granted.
        switch (nested->decoration_mode()) {
        case luminaria::HostDecorationMode::ServerSide:
            std::printf("tinyluminaria: window decoration = native (server-side)\n");
            break;
        case luminaria::HostDecorationMode::ClientSide:
            std::printf("tinyluminaria: window decoration = none "
                        "(parent insists on client-side; we don't draw one)\n");
            break;
        case luminaria::HostDecorationMode::None:
            std::printf("tinyluminaria: window decoration = none "
                        "(parent has no xdg-decoration global)\n");
            break;
        }
    }

    luminaria::EventSource exit_timer;
    if (const char* ms = std::getenv("LUMINARIA_EXIT_MS")) {
        exit_timer = display.event_loop().add_timer([&] { display.terminate(); });
        exit_timer.arm(static_cast<unsigned>(std::atoi(ms)));
    }

    display.run();
    return 0;
}
