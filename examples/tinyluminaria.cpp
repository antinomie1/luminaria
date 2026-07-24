// tinyluminaria — a minimal reference compositor built on luminaria. Mirrors tinywl: wire
// up the backend, compositor, xdg-shell, seat, and scene, then run the loop.
//
// Runs on the headless backend (no GPU/display needed to smoke-test the wiring).
// Env knobs: LUMINARIA_BACKEND=headless forces headless, LUMINARIA_OUTPUT=WxH
// sets the output size, LUMINARIA_EXIT_MS auto-terminates after N ms (smoke test).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "luminaria/backend/headless.hpp"
#include "luminaria/backend/wayland.hpp"
#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"
#include "luminaria/data_device.hpp"
#include "luminaria/drm_syncobj.hpp"
#include "luminaria/linux_dmabuf.hpp"
#include "luminaria/output_global.hpp"
#include "luminaria/render/vulkan.hpp"
#include "luminaria/screencopy.hpp"
#include "luminaria/seat.hpp"
#include "luminaria/subcompositor.hpp"
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
    // Tell clients how big a window we recommend (xdg_toplevel.configure_bounds).
    shell.set_bounds(output_width, output_height);
    // Clipboard, drag-and-drop, and middle-click paste. Both follow seat focus.
    auto data_device = must(luminaria::DataDeviceManager::create(display, seat), "data-device");
    auto primary_selection =
        must(luminaria::PrimarySelectionManager::create(display, seat), "primary-selection");
    // Real clients (weston-terminal) won't map a window until they see an output.
    auto output_global =
        must(luminaria::OutputGlobal::create(display, output_width, output_height), "wl_output");

    // Screencopy: allow tools like grim/slurp to capture the output.
    auto screencopy = must(luminaria::ScreencopyManager::create(display), "screencopy");
    // Cache the last rendered frame so the capture callback can serve subregions.
    auto last_frame = std::make_shared<std::vector<luminaria::Pixel>>();
    output_global.on_bind([&](wl_resource* res) {
        screencopy.add_output(res, output_global.width(), output_global.height(),
            [last_frame, ow = output_global.width(), oh = output_global.height()]
            (int x, int y, int w, int h, std::vector<uint8_t>& rgba) -> bool {
                if (last_frame->empty()) return false;
                // Extract subregion from the cached full-frame pixels.
                rgba.resize(static_cast<size_t>(w) * h * 4);
                for (int row = 0; row < h; ++row) {
                    const auto* src = last_frame->data() + (y + row) * ow + x;
                    auto* dst = reinterpret_cast<uint8_t*>(rgba.data()) + row * w * 4;
                    std::memcpy(dst, src, static_cast<size_t>(w) * 4);
                }
                return true;
            });
    });

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
    const std::vector<std::uint8_t> default_cursor = make_default_cursor();
    std::vector<luminaria::Signal<luminaria::FrameEvent>::Connection> frame_conns;
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

    auto new_output = backend->new_output.connect([&](luminaria::NewOutput& e) {
        const int ow = e.output.width();
        const int oh = e.output.height();
        frame_conns.push_back(e.output.frame.connect([&, ow, oh](luminaria::FrameEvent& fe) {
            // Reap closed windows and popups here (safe point) — not in the
            // destroy callback, which would free the entry while its own slot
            // is running.
            std::erase_if(windows, [](const Window& w) { return w.toplevel == nullptr; });
            std::erase_if(popups, [](const PopupEntry& p) { return p.popup == nullptr; });

            // Composite mapped client windows onto the background, present the frame.
            // Falls back to a solid fill if there's no renderer or nothing to show.
            std::vector<std::vector<std::uint8_t>> holds;
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
                // The cursor goes on top of everything: the client's own sprite
                // if it set one, otherwise our built-in arrow.
                if (ptr_inside) {
                    if (luminaria::Surface* cursor = seat.cursor_surface(); cursor != nullptr) {
                        std::vector<std::uint8_t> rgba;
                        int bw = 0, bh = 0;
                        if (cursor->current_buffer_rgba(rgba, bw, bh) && bw > 0 && bh > 0) {
                            holds.push_back(std::move(rgba));
                            textures.push_back({ptr_x - seat.cursor_hotspot_x(),
                                                ptr_y - seat.cursor_hotspot_y(), bw, bh,
                                                holds.back().data()});
                        }
                    } else {
                        textures.push_back(
                            {ptr_x, ptr_y, kCursorW, kCursorH, default_cursor.data()});
                    }
                }
                static int frame_n = 0;
                if (++frame_n % 60 == 1) {
                    std::printf("tinyluminaria: frame %d — %zu window(s), %zu popup(s), "
                                "%zu texture(s)\n",
                                frame_n, windows.size(), popups.size(), textures.size());
                }
                if (auto px = renderer->composite(ow, oh, kBg, {}, textures)) {
                    // Cache the frame for screencopy clients.
                    *last_frame = *px;
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
            // Fallback: solid background. Also populate last_frame for screencopy.
            {
                last_frame->resize(static_cast<size_t>(ow) * oh);
                luminaria::Pixel bg{static_cast<uint8_t>(kBg.r * 255),
                                    static_cast<uint8_t>(kBg.g * 255),
                                    static_cast<uint8_t>(kBg.b * 255), 255};
                std::fill(last_frame->begin(), last_frame->end(), bg);
            }
            (void)fe.output.commit(kBg);
        }));
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
        w.on_map = e.toplevel.map.connect([&w, &focus_window](luminaria::ToplevelMap&) {
            w.mapped = true;
            focus_window(&w);
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
    if (nested) {
        // Topmost surface under (x,y), plus the point in its local coordinates.
        auto hit_test = [&](double x, double y, double& lx, double& ly) -> luminaria::Surface* {
            const std::vector<Layer> layers = build_layers();
            for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
                luminaria::Surface* s = it->surface;
                if (s->buffer_width() <= 0 || x < it->x || y < it->y ||
                    x >= it->x + s->buffer_width() || y >= it->y + s->buffer_height()) {
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
    }

    if (auto socket = display.add_socket_auto()) {
        setenv("WAYLAND_DISPLAY", socket->c_str(), 1);
        std::printf("tinyluminaria running on WAYLAND_DISPLAY=%s\n", socket->c_str());
    }

    (void)backend->start();

    if (nested) {
        // Decoration is negotiated during start(); report what the host granted.
        switch (nested->decoration_mode()) {
        case luminaria::DecorationMode::ServerSide:
            std::printf("tinyluminaria: window decoration = native (server-side)\n");
            break;
        case luminaria::DecorationMode::ClientSide:
            std::printf("tinyluminaria: window decoration = none "
                        "(parent insists on client-side; we don't draw one)\n");
            break;
        case luminaria::DecorationMode::None:
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
