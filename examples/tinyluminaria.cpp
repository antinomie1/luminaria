// tinyluminaria — a minimal reference compositor built on luminaria. Mirrors tinywl: wire
// up the backend, compositor, xdg-shell, seat, and scene, then run the loop.
//
// Runs on the headless backend (no GPU/display needed to smoke-test the wiring).
// Env knobs: LUMINARIA_BACKEND=headless forces headless, LUMINARIA_OUTPUT=WxH
// sets the output size, LUMINARIA_EXIT_MS auto-terminates after N ms (smoke test).
#include "luminaria/detail/wayland_fwd.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <drm_fourcc.h>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

import luminaria;

namespace {
template <class T>
T must(luminaria::Result<T> r, const char* what) {
    if (!r) {
        std::fprintf(stderr, "tinyluminaria: %s: %s\n", what, r.error().message.c_str());
        std::exit(1);
    }
    return std::move(*r);
}

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

/// One composited surface: a toplevel, one of its subsurfaces, or a popup,
/// already placed in output coordinates. Built back-to-front once per frame and
/// used for both rendering and hit-testing, so the two can never disagree.
struct Layer {
    luminaria::Surface* surface;
    int x, y;
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
    const char* cursor_name = "default";
    auto cursor_shape_conn =
        cursor_shape.request().connect([&](luminaria::CursorShapeRequest& r) {
            cursor_name = r.name;
        });

    // Screencopy: allow tools like grim/slurp to capture the output.
    auto screencopy = must(luminaria::ScreencopyManager::create(display), "screencopy");

    // Per-output state — each output owns its own frame cache, GPU scanout
    // target, and connections. Nothing is shared across outputs, so a hotplug
    // event can never corrupt another monitor's data.
    struct PerOutput {
        std::shared_ptr<std::vector<luminaria::Pixel>> last_frame =
            std::make_shared<std::vector<luminaria::Pixel>>();
        std::vector<std::uint8_t> readback; // reused every frame, never reallocated
        std::optional<luminaria::ScanoutTarget> scanout;
        luminaria::Signal<luminaria::OutputDestroy>::Connection on_destroy;
        luminaria::Signal<luminaria::FrameEvent>::Connection on_frame;
        luminaria::Signal<luminaria::PresentEvent>::Connection on_present;
    };
    std::map<luminaria::Output*, PerOutput> per_output;

    output_global.on_bind([&](wl_resource* res) {
        screencopy.add_output(res, output_global.width(), output_global.height(),
            [&per_output, ow = output_global.width(), oh = output_global.height()]
            (int x, int y, int w, int h, std::vector<uint8_t>& rgba) -> bool {
                if (per_output.empty()) return false;
                // Single-output compositor: use the first (only) output's frame.
                auto& frame = *per_output.begin()->second.last_frame;
                if (frame.empty()) return false;
                // Extract subregion from the cached full-frame pixels.
                rgba.resize(static_cast<size_t>(w) * h * 4);
                for (int row = 0; row < h; ++row) {
                    const auto* src = frame.data() + (y + row) * ow + x;
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
    luminaria::Surface* ptr_focus = nullptr; // surface under the cursor
    // Raw Surface* needs a destroy subscription, or a client that closes the
    // window under the cursor leaves us pointing at freed memory.
    luminaria::Signal<luminaria::SurfaceDestroy>::Connection ptr_focus_gone;
    auto set_ptr_focus = [&](luminaria::Surface* surface) {
        ptr_focus = surface;
        ptr_focus_gone.disconnect();
        if (surface != nullptr) {
            ptr_focus_gone =
                surface->destroy.connect([&](luminaria::SurfaceDestroy&) { ptr_focus = nullptr; });
        }
    };
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
    // Where the outputs sit relative to each other. One output here, but the
    // layout is what xdg-output and multi-monitor hit-testing quote.
    luminaria::OutputLayout layout;
    const luminaria::Color kBg{0.1f, 0.1f, 0.12f, 1.0f};

    // Everything visible, back-to-front, in output coordinates: toplevels with
    // their subsurface trees, then popups anchored to their parents.
    auto build_layers = [&] {
        std::vector<Layer> layers;
        std::map<luminaria::Surface*, std::pair<int, int>> origin;
        for (Window& w : windows) {
            if (!w.mapped || w.toplevel == nullptr) {
                continue;
            }
            luminaria::Surface& root = w.toplevel->surface();
            origin[&root] = {w.x, w.y};
            for (const luminaria::SurfaceAt& at : root.surface_tree()) {
                layers.push_back(Layer{at.surface, w.x + at.x, w.y + at.y});
            }
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
            for (const luminaria::SurfaceAt& at : root.surface_tree()) {
                layers.push_back(Layer{at.surface, px + at.x, py + at.y});
            }
        }
        return layers;
    };

    // Teach the shell where windows are, so xdg_positioner's constraint
    // adjustment can do its job: a menu near the bottom of the screen flips up
    // instead of hanging off the edge. Without this the shell has no idea where
    // on screen anything is and menus simply overhang.
    shell.set_popup_constraint_query([&](luminaria::Surface& parent, luminaria::Box& parent_box,
                                         luminaria::Box& usable) {
        for (const Layer& l : build_layers()) {
            if (l.surface != &parent) {
                continue;
            }
            parent_box = luminaria::Box{l.x, l.y, parent.surface_width(),
                                        parent.surface_height()};
            luminaria::Output* on = layout.at(l.x, l.y);
            usable = on != nullptr ? layout.box_of(*on) : layout.bounds();
            return !usable.empty();
        }
        return false;
    });

    auto new_output = backend->new_output.connect([&](luminaria::NewOutput& e) {
        const int ow = e.output.width();
        const int oh = e.output.height();
        layout.add_auto(e.output);
        const luminaria::Box placed = layout.box_of(e.output);
        output_global.set_logical_position(placed.x, placed.y);

        // Per-output state: each monitor owns its own frame cache and GPU
        // target. Cached across frames — allocating a scanout target every
        // frame would leak GPU memory.
        auto& po = per_output[&e.output];
        if (renderer) {
            if (auto t = renderer->create_scanout(ow, oh, DRM_FORMAT_XRGB8888, {})) {
                po.scanout.emplace(std::move(*t));
            } else {
                std::fprintf(stderr, "tinyluminaria: scanout target: %s\n",
                             t.error().message.c_str());
            }
        }
        // Output unplugged: drop it from the layout and free its state.
        po.on_destroy = e.output.destroy.connect([&](luminaria::OutputDestroy& ev) {
            layout.remove(ev.output);
            per_output.erase(&ev.output);
        });

        // A frame reached the screen: only now do the clients that drew it get
        // told to draw again, and only now is their presentation feedback true.
        po.on_present = e.output.present.connect([&](luminaria::PresentEvent& pe) {
            for (const Layer& layer : build_layers()) {
                layer.surface->send_frame_done(pe.time_ms());
                presentation.notify_presented(*layer.surface, pe);
            }
            if (luminaria::Surface* cursor = seat.cursor_surface(); cursor != nullptr) {
                cursor->send_frame_done(pe.time_ms());
            }
        });

        // --- frame callback: GPU path first, CPU fallback below ---
        po.on_frame = e.output.frame.connect([&, ow, oh, &po = po](luminaria::FrameEvent& fe) {
            // Reap closed windows and popups here (safe point) — not in the
            // destroy callback, which would free the entry while its own slot
            // is running.
            std::erase_if(windows, [](const Window& w) { return w.toplevel == nullptr; });
            std::erase_if(popups, [](const PopupEntry& p) { return p.popup == nullptr; });

            static int frame_n = 0;
            if (++frame_n % 60 == 1) {
                std::printf("tinyluminaria: frame %d — %zu window(s), %zu popup(s), "
                            "cursor '%s'\n",
                            frame_n, windows.size(), popups.size(), cursor_name);
            }

            // --- GPU compositing path ---
            // Client dmabufs are imported with zero copy; compositing and
            // blending happen entirely on the GPU. Only the final frame is read
            // back to the CPU (once, for screencopy & commit_frame), not every
            // client buffer. Falls through to the CPU path when the scanout
            // target is unavailable.
            if (renderer && po.scanout.has_value()) {
                // The textures are owned by the surfaces and cached there, so
                // this is a list of borrowed pointers — no per-frame re-import,
                // and nothing to keep alive here.
                std::vector<luminaria::GpuTextureFill> gpu_fills;

                for (const Layer& layer : build_layers()) {
                    luminaria::GpuTexture* tex = layer.surface->buffer_texture(*renderer);
                    if (tex == nullptr || tex->width() <= 0 || tex->height() <= 0) {
                        continue;
                    }
                    luminaria::GpuTextureFill fill;
                    fill.texture = tex;
                    fill.x = layer.x;
                    fill.y = layer.y;
                    fill.w = layer.surface->surface_width();
                    fill.h = layer.surface->surface_height();
                    fill.transform = layer.surface->buffer_transform();
                    layer.surface->buffer_source_uv(fill.u0, fill.v0, fill.u1, fill.v1);
                    const luminaria::Box extents = layer.surface->opaque_region().extents();
                    fill.opaque = luminaria::Box{layer.x + extents.x, layer.y + extents.y,
                                                 extents.width, extents.height};
                    gpu_fills.push_back(fill);
                }

                // Cursor on top: client sprite first, otherwise the built-in
                // arrow (CPU pixels — pushed as a one-off upload).
                if (ptr_inside) {
                    if (luminaria::Surface* cursor = seat.cursor_surface(); cursor != nullptr) {
                        luminaria::GpuTexture* tex = cursor->buffer_texture(*renderer);
                        if (tex != nullptr && tex->width() > 0 && tex->height() > 0) {
                            luminaria::GpuTextureFill fill;
                            fill.texture = tex;
                            fill.x = ptr_x - seat.cursor_hotspot_x();
                            fill.y = ptr_y - seat.cursor_hotspot_y();
                            fill.w = cursor->surface_width();
                            fill.h = cursor->surface_height();
                            fill.transform = cursor->buffer_transform();
                            cursor->buffer_source_uv(fill.u0, fill.v0, fill.u1, fill.v1);
                            gpu_fills.push_back(fill);
                        }
                    }
                    else if (ensure_cursor_texture()) {
                        luminaria::GpuTextureFill fill;
                        fill.texture = &*cursor_texture;
                        fill.x = ptr_x - cursor_texture_image->hotspot_x;
                        fill.y = ptr_y - cursor_texture_image->hotspot_y;
                        fill.w = cursor_texture_image->width;
                        fill.h = cursor_texture_image->height;
                        gpu_fills.push_back(fill);
                    }
                }

                // All client damage consumed this frame.
                for (const Layer& layer : build_layers()) {
                    layer.surface->clear_damage();
                }

                if (auto s = renderer->render_to(*po.scanout, kBg, {}, gpu_fills)) {
                    // Read the composited frame back once, for screencopy and
                    // commit_frame. This is the ONLY CPU readback on this path —
                    // client buffers never left the GPU. `read_scanout` copies
                    // from the target's own image through a staging buffer it
                    // keeps mapped; re-importing the dmabuf every frame instead
                    // cost ~90ms, which is exactly what a laggy pointer is.
                    if (auto r = renderer->read_scanout(*po.scanout, po.readback)) {
                        po.last_frame->resize(po.readback.size() / 4);
                        std::memcpy(po.last_frame->data(), po.readback.data(),
                                    po.readback.size());
                        if (auto s2 = fe.output.commit_frame(*po.last_frame, ow, oh); !s2) {
                            std::fprintf(stderr, "tinyluminaria: commit_frame: %s\n",
                                         s2.error().message.c_str());
                        }
                        return;
                    } else {
                        std::fprintf(stderr, "tinyluminaria: readback: %s\n",
                                     r.error().message.c_str());
                    }
                } else {
                    std::fprintf(stderr, "tinyluminaria: render_to: %s\n",
                                 s.error().message.c_str());
                }
                // GPU path failed; fall through to CPU path.
            }

            // --- CPU compositing fallback ---
            // std::list never moves existing elements — pointers into its
            // contents (TextureFill::rgba) stay valid across push_back.
            {
                std::list<std::vector<std::uint8_t>> holds;
                std::vector<luminaria::TextureFill> textures;
                if (renderer) {
                    for (const Layer& layer : build_layers()) {
                        std::vector<std::uint8_t> rgba;
                        int bw = 0, bh = 0;
                        if (layer.surface->current_buffer_rgba(rgba, bw, bh) && bw > 0 && bh > 0) {
                            holds.push_back(std::move(rgba));
                            textures.push_back({layer.x, layer.y, bw, bh, holds.back().data()});
                        }
                    }
                    if (ptr_inside) {
                        if (luminaria::Surface* cursor = seat.cursor_surface();
                            cursor != nullptr) {
                            std::vector<std::uint8_t> rgba;
                            int bw = 0, bh = 0;
                            if (cursor->current_buffer_rgba(rgba, bw, bh) && bw > 0 && bh > 0) {
                                holds.push_back(std::move(rgba));
                                textures.push_back({ptr_x - seat.cursor_hotspot_x(),
                                                    ptr_y - seat.cursor_hotspot_y(), bw, bh,
                                                    holds.back().data()});
                            }
                        } else if (const luminaria::CursorImage* image = themed_cursor();
                                   image != nullptr) {
                            textures.push_back({ptr_x - image->hotspot_x,
                                                ptr_y - image->hotspot_y, image->width,
                                                image->height, image->rgba.data()});
                        } else {
                            textures.push_back(
                                {ptr_x, ptr_y, kCursorW, kCursorH, builtin_cursor.data()});
                        }
                    }
                    for (const Layer& layer : build_layers()) {
                        layer.surface->clear_damage();
                    }
                    if (auto px = renderer->composite(ow, oh, kBg, {}, textures)) {
                        *po.last_frame = *px;
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
                // Solid background — the output still works, just isn't drawn.
                po.last_frame->resize(static_cast<size_t>(ow) * oh);
                luminaria::Pixel bg{static_cast<uint8_t>(kBg.r * 255),
                                    static_cast<uint8_t>(kBg.g * 255),
                                    static_cast<uint8_t>(kBg.b * 255), 255};
                std::fill(po.last_frame->begin(), po.last_frame->end(), bg);
                (void)fe.output.commit(kBg);
            }
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
            [&w, &focus_window, &fractional, &output_global](luminaria::ToplevelMap&) {
                w.mapped = true;
                focus_window(&w);
                // Tell the client what density to render at, both ways: the
                // integer hint every client understands, and the exact scale
                // for the ones that speak fractional-scale.
                luminaria::Surface& surface = w.toplevel->surface();
                surface.set_preferred_buffer_scale(output_global.scale());
                surface.set_preferred_buffer_transform(output_global.transform());
                fractional.set_scale(surface, output_global.scale() * 120);
            });
        w.on_unmap = e.toplevel.unmap.connect([&w](luminaria::ToplevelUnmap&) {
            w.mapped = false;
        });
        w.on_destroy = e.toplevel.destroy.connect(
            [&w, &focused, &focus_window](luminaria::ToplevelDestroy&) {
                if (focused == w.toplevel) {
                    focus_window(nullptr);
                }
                w.mapped = false;
                w.toplevel = nullptr;
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
        p.on_map = e.popup.map.connect([&p](luminaria::PopupMap&) { p.mapped = true; });
        p.on_unmap = e.popup.unmap.connect([&p](luminaria::PopupUnmap&) { p.mapped = false; });
        p.on_destroy = e.popup.destroy.connect([&p](luminaria::PopupDestroy&) {
            p.mapped = false;
            p.popup = nullptr;
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
        auto hit_test = [&](double x, double y, double& lx, double& ly) -> luminaria::Surface* {
            const std::vector<Layer> layers = build_layers();
            for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
                luminaria::Surface* s = it->surface;
                // accepts_input honours the client's input region, so a click in
                // a rounded corner or a shadow falls through to what's behind.
                if (!s->accepts_input(x - it->x, y - it->y)) {
                    continue;
                }
                lx = x - it->x;
                ly = y - it->y;
                return s;
            }
            return nullptr;
        };
        // The window (if any) a surface belongs to, so a click can raise+focus it.
        auto window_of = [&](luminaria::Surface* surface) -> Window* {
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
        auto is_popup_surface = [&](luminaria::Surface* surface) {
            for (PopupEntry& p : popups) {
                if (!p.mapped || p.popup == nullptr) {
                    continue;
                }
                for (const luminaria::SurfaceAt& at : p.popup->surface().surface_tree()) {
                    if (at.surface == surface) {
                        return true;
                    }
                }
            }
            return false;
        };

        on_ptr_motion = nested->pointer_motion.connect([&](luminaria::PointerMotionAbsEvent& e) {
            if (e.x < 0) { // pointer left our window
                ptr_inside = false;
                set_ptr_focus(nullptr);
                seat.pointer_clear_focus();
                return;
            }
            ptr_inside = true;
            ptr_x = static_cast<int>(e.x);
            ptr_y = static_cast<int>(e.y);
            double lx = 0, ly = 0;
            luminaria::Surface* hit = hit_test(e.x, e.y, lx, ly);
            if (hit == nullptr) {
                set_ptr_focus(nullptr);
                seat.pointer_clear_focus();
                return;
            }
            if (hit != ptr_focus) {
                set_ptr_focus(hit);
                seat.pointer_enter(*hit, lx, ly);
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
            if (ptr_focus == nullptr) {
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
