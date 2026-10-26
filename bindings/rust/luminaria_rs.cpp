// luminaria_rs.cpp — the C++ half of the Rust binding, and the only place the
// modules appear. xmake compiles it (clang + C++ modules); cc never sees it.
//
// This is an ordinary translation unit that imports a module, not a module
// unit, so it has no `module;` line: includes first, then the import, exactly
// like examples/tinyluminaria.cpp.
//
// What it is: tinyluminaria's wiring with the policy taken out. Everything
// every compositor writes the same way — globals, renderer, frame pump, frame
// callbacks, presentation feedback, popups, the cursor, pointer focus — is
// done here. What a window manager decides — where windows go, their stacking,
// sizes, focus, bindings — is Rust's, and arrives as plain calls.
//
// The rule that keeps it sound: C++ never calls Rust. libwayland's callbacks
// only append to `events`; Rust drains that queue after `dispatch()` returns,
// and every Rust call happens while no luminaria signal is on the stack. So
// there is no re-entrancy to reason about, and Rust can hold `&mut Server`
// for as long as it likes.
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#include <drm_fourcc.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "luminaria/luminaria_rs.h"
#include "luminaria/src/lib.rs.h"

import luminaria.gpu;

namespace lum {

namespace lm = luminaria;

template <class E>
using Conn = typename lm::Signal<E>::Connection;

namespace {

std::uint32_t now_ms() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

void warn(const char* what, const lm::Error& error) {
    std::fprintf(stderr, "luminaria-rs: %s: %s\n", what, error.message.c_str());
}

// Client strings are not guaranteed UTF-8, and rust::String's checked
// constructor throws on bad input. Lossy never throws.
rust::String to_rust(const std::string& s) { return rust::String::lossy(s); }

// The xkb modifier bits a binding may name. Fixed by xkb's core modifiers, so
// they hold for every keymap: Shift, Control, Mod1 (Alt), Mod4 (Logo). Locks
// (Caps, Num) are deliberately not here — a binding must not stop working
// because Caps Lock is on.
constexpr std::uint32_t kBindableMods = (1u << 0) | (1u << 2) | (1u << 3) | (1u << 6);

} // namespace

// Named, not anonymous: these are members of Server::Impl, which has external
// linkage, and gcc warns (-Wsubobject-linkage, fatal here) about a field whose
// type is TU-local.
namespace detail {

struct Window {
    std::uint64_t id = 0;
    lm::Toplevel* toplevel = nullptr; // null once the client destroyed it
    bool mapped = false;
    bool dead = false; // reaped at the next dispatch, never inside its own slot
    Conn<lm::ToplevelMap> on_map;
    Conn<lm::ToplevelUnmap> on_unmap;
    Conn<lm::ToplevelDestroy> on_destroy;
    Conn<lm::ToplevelRequestMaximize> on_maximize;
    Conn<lm::ToplevelRequestFullscreen> on_fullscreen;
    Conn<lm::ToplevelRequestMinimize> on_minimize;
    Conn<lm::ToplevelRequestMove> on_move;
    Conn<lm::ToplevelRequestResize> on_resize;
    Conn<lm::ToplevelIdentityChange> on_identity;
};

struct PopupEntry {
    lm::Popup* popup = nullptr;
    bool dead = false;
    Conn<lm::PopupMap> on_map;
    Conn<lm::PopupUnmap> on_unmap;
    Conn<lm::PopupDestroy> on_destroy;
};

struct OutputEntry {
    std::uint64_t id = 0;
    lm::Output* output = nullptr; // null once unplugged
    bool dead = false;
    lm::SceneOutput state;
    std::optional<lm::OutputGlobal> global;
    std::vector<lm::SurfaceId> drawn; // roots of the last frame, for presentation feedback
    Conn<lm::FrameEvent> on_frame;
    Conn<lm::PresentEvent> on_present;
    Conn<lm::OutputDestroy> on_destroy;
};

struct SurfaceWatch {
    lm::SurfaceId surface;
    Conn<lm::SurfaceCommit> on_commit;
};

struct Binding {
    std::uint32_t id;
    std::uint32_t mods;
    std::uint32_t keysym;
};

} // namespace detail

using namespace detail;

struct Server::Impl {
    // Declaration order is destruction order, reversed, and three of these
    // depend on it:
    //  * `renderer` first, so it dies last: every SceneOutput below caches GPU
    //    textures it owns ("the renderer must outlive the Display").
    //  * `display` before the globals and the backend, which hold its loop.
    //  * the per-window/per-output state last, so it dies first: its signal
    //    connections are gone before any object they point into.
    lm::SceneRenderer renderer;
    std::optional<lm::Display> display;

    std::optional<lm::Compositor> compositor;
    std::optional<lm::Subcompositor> subcompositor;
    std::optional<lm::XdgShell> shell;
    std::optional<lm::Seat> seat;
    std::optional<lm::SinglePixelBufferManager> single_pixel;
    std::optional<lm::Presentation> presentation;
    std::optional<lm::Viewporter> viewporter;
    std::optional<lm::FractionalScaleManager> fractional;
    std::optional<lm::XdgDecorationManager> decoration;
    std::optional<lm::CursorShapeManager> cursor_shape;
    std::optional<lm::DataDeviceManager> data_device;
    std::optional<lm::PrimarySelectionManager> primary_selection;
    std::optional<lm::FifoManager> fifo;
    std::optional<lm::CommitTimingManager> commit_timing;
    std::optional<lm::LinuxDmabuf> dmabuf;
    std::optional<lm::DrmSyncobjManager> syncobj;

    std::unique_ptr<lm::Backend> backend;
    lm::WaylandBackend* nested = nullptr; // == backend.get() when nested; input comes from it

    std::string socket;
    lm::OutputLayout layout;
    lm::Pointer pointer;
    lm::CursorManager cursors;
    lm::PopupTree popup_tree;
    std::optional<lm::KeymapState> keymap;
    lm::KeyRouter keys;
    lm::Color background{0.1f, 0.1f, 0.12f, 1.0f};

    std::vector<Item> items;            // Rust's list, global logical coordinates
    std::vector<lm::SceneItem> scene;   // `items` resolved, plus popups (global)
    std::vector<lm::SceneItem> local;   // `scene` in one output's coordinates
    std::vector<lm::SurfaceAt> tree;    // scratch for surface_tree()
    std::deque<Event> events;
    std::vector<Binding> bindings;

    std::uint64_t next_window = 1;
    std::uint64_t next_output = 1;
    std::uint64_t focused = 0;
    lm::SurfaceId ptr_focus;
    std::uint64_t ptr_window = 0;
    int buttons_down = 0;

    Conn<lm::NewOutput> on_new_output;
    Conn<lm::NewToplevel> on_new_toplevel;
    Conn<lm::NewPopup> on_new_popup;
    Conn<lm::NewSurface> on_new_surface;
    Conn<lm::CursorShapeRequest> on_cursor_shape;
    Conn<lm::SeatCursorChange> on_cursor_change;
    Conn<lm::PointerMotionAbsEvent> on_motion;
    Conn<lm::PointerButtonEvent> on_button;
    Conn<lm::PointerAxisEvent> on_axis;
    Conn<lm::KeyEvent> on_key;
    Conn<lm::ModifiersEvent> on_mods;
    Conn<lm::KeymapChange> on_keymap;
    Conn<lm::HostCloseRequest> on_close;

    std::vector<SurfaceWatch> surfaces;
    std::vector<std::unique_ptr<OutputEntry>> outputs;
    std::vector<std::unique_ptr<PopupEntry>> popups;
    std::vector<std::unique_ptr<Window>> windows;

    // ------------------------------------------------------------- helpers

    void push(EventKind kind, std::uint64_t window = 0) {
        Event e{};
        e.kind = kind;
        e.window = window;
        events.push_back(e);
    }

    Window* window(std::uint64_t id) const {
        for (const auto& w : windows) {
            if (w->id == id && w->toplevel != nullptr) {
                return w.get();
            }
        }
        return nullptr;
    }

    /// Nothing reports damage for a layout change, a popup or the cursor; ask
    /// every output for a frame and let the scene diff work out what moved.
    void schedule_all() {
        for (const auto& o : outputs) {
            if (o->output != nullptr) {
                o->output->schedule_frame();
            }
        }
    }

    /// `items` → `scene`. A window item names where its *window geometry* goes
    /// (what xdg-shell calls the window, without CSD shadows), and is clipped
    /// to it. That is also the box popups are positioned against, so a menu
    /// lands where the client asked even on a shadowed GTK window.
    void build_scene() {
        scene.clear();
        for (const Item& it : items) {
            switch (it.kind) {
            case ItemKind::Window: {
                const Window* w = window(it.window);
                if (w == nullptr || !w->mapped) {
                    break;
                }
                lm::Surface& root = w->toplevel->surface();
                const lm::XdgGeometry g = w->toplevel->geometry();
                const bool has_geometry = g.width > 0 && g.height > 0;
                const int ox = it.x - (has_geometry ? g.x : 0);
                const int oy = it.y - (has_geometry ? g.y : 0);
                const lm::Box box{it.x, it.y, has_geometry ? g.width : root.surface_width(),
                                  has_geometry ? g.height : root.surface_height()};
                scene.push_back({.kind = lm::SceneItem::Kind::surface,
                                 .box = box,
                                 .surface = root.id(),
                                 .x = ox,
                                 .y = oy,
                                 .tag = it.window});
                break;
            }
            case ItemKind::Rect:
                scene.push_back({.kind = lm::SceneItem::Kind::rect,
                                 .box = {it.x, it.y, it.width, it.height},
                                 .color = {it.color.r, it.color.g, it.color.b, it.color.a},
                                 .accepts_input = false});
                break;
            case ItemKind::Border:
                scene.push_back({.kind = lm::SceneItem::Kind::border,
                                 .box = {it.x, it.y, it.width, it.height},
                                 .color = {it.color.r, it.color.g, it.color.b, it.color.a},
                                 .thickness = it.thickness,
                                 .accepts_input = false});
                break;
            default:
                break;
            }
        }
        popup_tree.append_to(scene);
    }

    /// Global origin of `id` if it is drawn in `scene`, subsurfaces included.
    bool surface_origin(lm::SurfaceId id, double& x, double& y) {
        for (const lm::SceneItem& item : scene) {
            lm::Surface* root = item.kind == lm::SceneItem::Kind::surface
                                    ? lm::surface_from_id(item.surface)
                                    : nullptr;
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
    }

    /// Send pointer enter/motion for the current position. While a button is
    /// held the surface that took the press keeps the pointer (the implicit
    /// grab every toolkit's drag and text selection relies on).
    void update_pointer_focus() {
        if (!pointer.present()) {
            return;
        }
        build_scene();
        if (buttons_down > 0 && ptr_focus.valid()) {
            double ox = 0, oy = 0;
            if (lm::surface_from_id(ptr_focus) != nullptr && surface_origin(ptr_focus, ox, oy)) {
                seat->pointer_motion(pointer.x() - ox, pointer.y() - oy);
                return;
            }
        }
        const lm::SceneHit hit = lm::scene_hit_test(scene, pointer.x(), pointer.y());
        ptr_window = hit.hit ? hit.tag : 0;
        if (hit.surface == nullptr) {
            if (ptr_focus.valid()) {
                ptr_focus = {};
                seat->pointer_clear_focus();
            }
            return;
        }
        if (hit.surface->id() != ptr_focus) {
            ptr_focus = hit.surface->id();
            seat->pointer_enter(*hit.surface, hit.sx, hit.sy);
        } else {
            seat->pointer_motion(hit.sx, hit.sy);
        }
    }

    void draw(OutputEntry& o) {
        if (o.output == nullptr) {
            return;
        }
        build_scene();
        const lm::Box view = layout.box_of(*o.output);
        local.clear();
        o.drawn.clear();
        for (lm::SceneItem item : scene) {
            item.box.x -= view.x;
            item.box.y -= view.y;
            item.x -= view.x;
            item.y -= view.y;
            if (item.kind == lm::SceneItem::Kind::surface) {
                o.drawn.push_back(item.surface);
            }
            local.push_back(item);
        }
        lm::SceneCursor cursor = cursors.sprite(pointer, *seat);
        cursor.x -= view.x;
        cursor.y -= view.y;

        auto outcome = renderer.present(o.state, *o.output, local, background, cursor);
        if (!outcome) {
            warn("present", outcome.error());
        } else if (*outcome == lm::SceneOutcome::unchanged) {
            // Nothing was committed, so no `present` follows: release the
            // callbacks here or a client that committed without damage freezes.
            renderer.send_frame_done(o.state, now_ms());
        }
    }

    void presented(OutputEntry& o, const lm::PresentEvent& pe) {
        renderer.presented(o.state);
        renderer.send_frame_done(o.state, pe.time_ms());
        for (const lm::SurfaceId id : o.drawn) {
            lm::Surface* root = lm::surface_from_id(id);
            if (root == nullptr) {
                continue;
            }
            tree.clear();
            root->surface_tree(tree);
            for (const lm::SurfaceAt& at : tree) {
                presentation->notify_presented(*at.surface, pe);
            }
        }
    }

    /// Free everything whose object died during the last dispatch. Here and
    /// not in the destroy slots: a slot must not destroy the state it runs in.
    void reap() {
        std::erase_if(windows, [](const auto& w) { return w->dead; });
        std::erase_if(popups, [](const auto& p) { return p->dead; });
        std::erase_if(outputs, [](const auto& o) { return o->dead; });
        std::erase_if(surfaces,
                      [](const SurfaceWatch& s) { return lm::surface_from_id(s.surface) == nullptr; });
    }

    // ------------------------------------------------------------- wiring

    void watch_toplevel(lm::Toplevel& toplevel) {
        auto owned = std::make_unique<Window>();
        Window* w = owned.get();
        w->id = next_window++;
        w->toplevel = &toplevel;
        w->on_map = toplevel.map.connect([this, w](lm::ToplevelMap&) {
            w->mapped = true;
            // Render at the output's density, both the integer hint and the
            // exact scale for clients that speak fractional-scale.
            if (!outputs.empty() && outputs.front()->global.has_value()) {
                lm::OutputGlobal& g = *outputs.front()->global;
                lm::Surface& surface = w->toplevel->surface();
                surface.set_preferred_buffer_scale(g.scale());
                surface.set_preferred_buffer_transform(g.transform());
                fractional->set_scale(surface, g.scale() * 120);
            }
            push(EventKind::WindowMap, w->id);
            schedule_all();
        });
        w->on_unmap = toplevel.unmap.connect([this, w](lm::ToplevelUnmap&) {
            w->mapped = false;
            if (focused == w->id) {
                focused = 0;
                seat->set_keyboard_focus(nullptr);
            }
            push(EventKind::WindowUnmap, w->id);
            schedule_all();
        });
        w->on_destroy = toplevel.destroy.connect([this, w](lm::ToplevelDestroy&) {
            if (focused == w->id) {
                focused = 0;
                seat->set_keyboard_focus(nullptr);
            }
            w->mapped = false;
            w->toplevel = nullptr;
            w->dead = true;
            push(EventKind::WindowDestroy, w->id);
            schedule_all();
        });
        w->on_maximize = toplevel.request_maximize.connect(
            [this, w](lm::ToplevelRequestMaximize& e) {
                push(EventKind::RequestMaximize, w->id);
                events.back().on = e.maximized;
            });
        w->on_fullscreen = toplevel.request_fullscreen.connect(
            [this, w](lm::ToplevelRequestFullscreen& e) {
                push(EventKind::RequestFullscreen, w->id);
                events.back().on = e.fullscreen;
            });
        w->on_minimize = toplevel.request_minimize.connect(
            [this, w](lm::ToplevelRequestMinimize&) { push(EventKind::RequestMinimize, w->id); });
        w->on_move = toplevel.request_move.connect(
            [this, w](lm::ToplevelRequestMove&) { push(EventKind::RequestMove, w->id); });
        w->on_resize = toplevel.request_resize.connect([this, w](lm::ToplevelRequestResize& e) {
            push(EventKind::RequestResize, w->id);
            events.back().code = e.edges;
        });
        w->on_identity = toplevel.identity_change.connect(
            [this, w](lm::ToplevelIdentityChange&) { push(EventKind::WindowTitle, w->id); });
        windows.push_back(std::move(owned));
        push(EventKind::WindowNew, w->id);
    }

    void watch_popup(lm::Popup& popup) {
        auto owned = std::make_unique<PopupEntry>();
        PopupEntry* p = owned.get();
        p->popup = &popup;
        popup_tree.add(popup);
        p->on_map = popup.map.connect([this, p](lm::PopupMap&) {
            popup_tree.set_mapped(*p->popup, true);
            schedule_all();
        });
        p->on_unmap = popup.unmap.connect([this, p](lm::PopupUnmap&) {
            popup_tree.set_mapped(*p->popup, false);
            schedule_all();
        });
        p->on_destroy = popup.destroy.connect([this, p](lm::PopupDestroy&) {
            popup_tree.remove(*p->popup);
            p->popup = nullptr;
            p->dead = true;
            schedule_all();
        });
        popups.push_back(std::move(owned));
    }

    void watch_output(lm::Output& output) {
        auto owned = std::make_unique<OutputEntry>();
        OutputEntry* o = owned.get();
        o->id = next_output++;
        o->output = &output;
        layout.add_auto(output);
        const lm::Box box = layout.box_of(output);

        if (auto s = renderer.reset(o->state, output, DRM_FORMAT_XRGB8888); !s) {
            warn("render target (falling back to CPU)", s.error());
        }
        auto global = lm::OutputGlobal::create(*display, output.width(), output.height(),
                                               "luminaria-" + std::to_string(o->id));
        if (global) {
            global->set_logical_position(box.x, box.y);
            o->global.emplace(std::move(*global));
        } else {
            warn("wl_output", global.error());
        }
        shell->set_bounds(box.width, box.height);

        o->on_frame = output.frame.connect([this, o](lm::FrameEvent&) { draw(*o); });
        o->on_present =
            output.present.connect([this, o](lm::PresentEvent& pe) { presented(*o, pe); });
        o->on_destroy = output.destroy.connect([this, o](lm::OutputDestroy&) {
            layout.remove(*o->output);
            // The render state goes now, while the Output it was built for
            // still exists; the entry itself waits for reap().
            o->state = lm::SceneOutput{};
            o->global.reset();
            o->output = nullptr;
            o->dead = true;
            push(EventKind::OutputDestroy);
            events.back().output = o->id;
        });
        outputs.push_back(std::move(owned));

        Event e{};
        e.kind = EventKind::OutputNew;
        e.output = o->id;
        e.x = box.x;
        e.y = box.y;
        e.width = box.width;
        e.height = box.height;
        events.push_back(e);
        output.schedule_frame();
    }

    void watch_input(lm::WaylandBackend& host) {
        on_motion = host.pointer_motion.connect([this](lm::PointerMotionAbsEvent& e) {
            schedule_all(); // the cursor is composited; it moving is a repaint
            if (e.x < 0) {  // left the host window
                pointer.leave();
                ptr_focus = {};
                ptr_window = 0;
                seat->pointer_clear_focus();
                return;
            }
            const lm::Box origin = outputs.empty() || outputs.front()->output == nullptr
                                       ? lm::Box{}
                                       : layout.box_of(*outputs.front()->output);
            pointer.move_to(origin.x + e.x, origin.y + e.y, layout.bounds());
            update_pointer_focus();
            Event ev{};
            ev.kind = EventKind::PointerMotion;
            ev.window = ptr_window;
            ev.x = pointer.x();
            ev.y = pointer.y();
            events.push_back(ev);
        });
        on_button = host.pointer_button.connect([this](lm::PointerButtonEvent& e) {
            if (e.pressed) {
                (void)popup_tree.dismiss_grabs_outside(ptr_focus); // menu semantics
            }
            buttons_down = std::max(0, buttons_down + (e.pressed ? 1 : -1));
            Event ev{};
            ev.kind = EventKind::PointerButton;
            ev.window = ptr_window;
            ev.code = e.button;
            ev.on = e.pressed;
            ev.x = pointer.x();
            ev.y = pointer.y();
            events.push_back(ev);
            if (ptr_focus.valid()) {
                seat->pointer_button(e.button, e.pressed);
            }
            if (!e.pressed && buttons_down == 0) {
                update_pointer_focus(); // the grab ended; re-pick what is under it
            }
        });
        on_axis = host.pointer_axis.connect([this](lm::PointerAxisEvent& e) {
            // Notches win over the smooth deltas that accompany them.
            if (e.dx_steps != 0 || e.dy_steps != 0) {
                seat->pointer_axis_discrete(e.dx_steps, e.dy_steps);
            } else if (e.dx != 0.0 || e.dy != 0.0) {
                seat->pointer_axis(e.dx, e.dy);
            }
            if (e.stop_horizontal || e.stop_vertical) {
                seat->pointer_axis_stop(e.stop_horizontal, e.stop_vertical);
            }
        });
        on_key = host.key.connect([this](lm::KeyEvent& e) { key(e.keycode, e.pressed); });
        on_mods = host.modifiers.connect([this](lm::ModifiersEvent& e) {
            if (keymap.has_value()) {
                lm::KeyRouter::sync_modifiers(*keymap, *seat, e);
            } else {
                seat->notify_modifiers(e.depressed, e.latched, e.locked, e.group);
            }
        });
        // Adopt the host's layout: the modifier masks above are computed
        // against it, and clients should see the layout the user types on.
        on_keymap = host.keymap_changed.connect([this](lm::KeymapChange& e) { use_keymap(e.text); });
        on_close = host.close_requested.connect(
            [this](lm::HostCloseRequest&) { push(EventKind::CloseRequested); });
    }

    void use_keymap(const std::string& text) {
        if (auto k = lm::KeymapState::from_text(text); k) {
            keymap.emplace(std::move(*k));
            if (!seat->set_keymap(text)) {
                std::fprintf(stderr, "luminaria-rs: host keymap rejected by the seat\n");
            }
        } else {
            warn("host keymap", k.error());
        }
    }

    void key(std::uint32_t keycode, bool pressed) {
        if (!pressed) {
            if (!keys.unconsume(keycode)) { // the release of a binding stays ours
                seat->notify_key(keycode, false);
            }
            return;
        }
        if (keymap.has_value() && !bindings.empty()) {
            const lm::ModifiersEvent m = keymap->modifiers();
            const std::uint32_t mods = (m.depressed | m.latched) & kBindableMods;
            const std::uint32_t sym = xkb_keysym_to_lower(keymap->keysym(keycode));
            for (const Binding& b : bindings) {
                if (b.mods == mods && b.keysym == sym) {
                    keys.consume(keycode);
                    push(EventKind::Binding);
                    events.back().code = b.id;
                    return;
                }
            }
        }
        seat->notify_key(keycode, true);
    }

    std::string init(const Config& config) {
        auto d = lm::Display::create();
        if (!d) {
            return "display: " + d.error().message;
        }
        display.emplace(std::move(*d));
        if (auto s = display->init_shm(); !s) {
            return "wl_shm: " + s.error().message;
        }

        // Every global a desktop client expects before it will map a window.
        // A failure in any of them is a broken install, not a runtime choice.
#define LUM_CREATE(member, expr, what)                                                            \
    do {                                                                                          \
        auto r = (expr);                                                                          \
        if (!r) {                                                                                 \
            return std::string(what) + ": " + r.error().message;                                  \
        }                                                                                         \
        member.emplace(std::move(*r));                                                            \
    } while (0)
        LUM_CREATE(compositor, lm::Compositor::create(*display), "wl_compositor");
        LUM_CREATE(subcompositor, lm::Subcompositor::create(*display), "wl_subcompositor");
        LUM_CREATE(shell, lm::XdgShell::create(*display), "xdg-shell");
        LUM_CREATE(seat, lm::Seat::create(*display), "wl_seat");
        LUM_CREATE(single_pixel, lm::SinglePixelBufferManager::create(*display),
                   "single-pixel-buffer");
        LUM_CREATE(presentation, lm::Presentation::create(*display), "presentation-time");
        LUM_CREATE(viewporter, lm::Viewporter::create(*display), "wp_viewporter");
        LUM_CREATE(fractional, lm::FractionalScaleManager::create(*display), "fractional-scale");
        LUM_CREATE(decoration, lm::XdgDecorationManager::create(*display), "xdg-decoration");
        LUM_CREATE(cursor_shape, lm::CursorShapeManager::create(*display), "cursor-shape");
        LUM_CREATE(data_device, lm::DataDeviceManager::create(*display, *seat), "data-device");
        LUM_CREATE(primary_selection, lm::PrimarySelectionManager::create(*display, *seat),
                   "primary-selection");
        LUM_CREATE(fifo, lm::FifoManager::create(*display), "wp-fifo");
        LUM_CREATE(commit_timing, lm::CommitTimingManager::create(*display), "wp-commit-timing");
#undef LUM_CREATE

        // Optional: only with a GPU, and the compositor works without either.
        if (renderer.gpu() != nullptr) {
            if (auto r = lm::LinuxDmabuf::create(*display, renderer.gpu()); r) {
                dmabuf.emplace(std::move(*r));
            } else {
                warn("linux-dmabuf (off)", r.error());
            }
        }
        if (auto r = lm::DrmSyncobjManager::create(*display); r) {
            syncobj.emplace(std::move(*r));
        }

        decoration->set_default_mode(config.server_side_decorations
                                         ? lm::DecorationMode::server_side
                                         : lm::DecorationMode::client_side);
        if (auto c = lm::CursorManager::create(); c) {
            cursors = std::move(*c);
        } // no theme installed: the cursor is whatever clients set, or none

        on_new_toplevel = shell->new_toplevel().connect(
            [this](lm::NewToplevel& e) { watch_toplevel(e.toplevel); });
        on_new_popup =
            shell->new_popup().connect([this](lm::NewPopup& e) { watch_popup(e.popup); });
        // Without a GPU no Frame is watching surfaces, so a commit would never
        // reach the screen. Asking twice on the GPU path costs nothing.
        on_new_surface = compositor->new_surface().connect([this](lm::NewSurface& e) {
            surfaces.push_back({e.surface.id(), e.surface.commit.connect(
                                                    [this](lm::SurfaceCommit&) { schedule_all(); })});
        });
        on_cursor_shape = cursor_shape->request().connect([this](lm::CursorShapeRequest& r) {
            cursors.set_shape(r.name);
            schedule_all();
        });
        on_cursor_change = seat->cursor_changed().connect([this](lm::SeatCursorChange&) {
            cursors.set_surface_cursor();
            schedule_all();
        });
        // xdg_positioner's constraint solving needs to know where the parent
        // is and what screen it is on, or menus hang off the edge.
        shell->set_popup_constraint_query([this](lm::Surface& parent, lm::Box& parent_box,
                                                 lm::Box& usable) {
            build_scene();
            for (const lm::SceneItem& item : scene) {
                if (item.kind != lm::SceneItem::Kind::surface || item.surface != parent.id()) {
                    continue;
                }
                parent_box = item.box;
                lm::Output* on = layout.at(item.box.x, item.box.y);
                usable = on != nullptr ? layout.box_of(*on) : layout.bounds();
                return !usable.empty();
            }
            return false;
        });

        // Backend: a window in the parent compositor when there is one,
        // otherwise headless.
        const int width = config.width > 0 ? config.width : 1280;
        const int height = config.height > 0 ? config.height : 800;
        if (config.backend != BackendKind::Headless) {
            if (auto wb = lm::WaylandBackend::create(display->event_loop()); wb) {
                auto owned = std::make_unique<lm::WaylandBackend>(std::move(*wb));
                nested = owned.get();
                nested->add_output(width, height, "luminaria");
                backend = std::move(owned);
            } else if (config.backend == BackendKind::Nested) {
                return "nested backend: " + wb.error().message;
            }
        }
        if (backend == nullptr) {
            auto owned = std::make_unique<lm::HeadlessBackend>(display->event_loop());
            owned->add_output(width, height);
            backend = std::move(owned);
        }
        on_new_output =
            backend->new_output.connect([this](lm::NewOutput& e) { watch_output(e.output); });
        if (nested != nullptr) {
            watch_input(*nested);
        }

        auto socket_name = display->add_socket_auto();
        if (!socket_name) {
            return "socket: " + socket_name.error().message;
        }
        socket = std::move(*socket_name);

        if (auto s = backend->start(); !s) {
            return "backend: " + s.error().message;
        }
        if (nested != nullptr && !nested->keymap().empty()) {
            use_keymap(nested->keymap()); // already arrived during start()
        } else if (auto k = lm::KeymapState::from_layout(); k) {
            keymap.emplace(std::move(*k));
        }
        return {};
    }

    ~Impl() {
        // Explicit, rather than trusting the member order alone: per-object
        // state first, so no slot can fire into a half-destroyed Impl while
        // the Display tears its clients down.
        windows.clear();
        popups.clear();
        outputs.clear();
        surfaces.clear();
    }
};

// ------------------------------------------------------------------ Server

Server::Server(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Server::~Server() = default;

rust::String Server::socket_name() const { return to_rust(impl_->socket); }

int Server::fd() const { return wl_event_loop_get_fd(impl_->display->event_loop().c_ptr()); }

bool Server::dispatch(int timeout_ms) {
    Impl& self = *impl_;
    self.reap();
    wl_display* d = self.display->c_ptr();
    // Whatever Rust asked for since the last dispatch goes out before we block.
    wl_display_flush_clients(d);
    if (!self.events.empty()) {
        timeout_ms = 0; // undrained events: do not sleep on them
    }
    const int r = wl_event_loop_dispatch(self.display->event_loop().c_ptr(), timeout_ms);
    wl_display_flush_clients(d);
    return r >= 0 || errno == EINTR;
}

bool Server::next_event(Event& out) {
    if (impl_->events.empty()) {
        return false;
    }
    out = impl_->events.front();
    impl_->events.pop_front();
    return true;
}

void Server::set_scene(rust::Slice<const Item> items) {
    impl_->items.assign(items.begin(), items.end());
    impl_->schedule_all();
    impl_->update_pointer_focus(); // a window may have moved under a still pointer
}

void Server::set_background(float r, float g, float b, float a) {
    impl_->background = {r, g, b, a};
    impl_->schedule_all();
}

void Server::focus(std::uint64_t id) {
    Impl& self = *impl_;
    Window* next = self.window(id);
    const std::uint64_t next_id = next != nullptr ? id : 0;
    if (self.focused == next_id) {
        return;
    }
    if (Window* old = self.window(self.focused); old != nullptr) {
        old->toplevel->set_activated(false);
    }
    self.focused = next_id;
    if (next != nullptr) {
        next->toplevel->set_activated(true);
        self.seat->set_keyboard_focus(&next->toplevel->surface());
    } else {
        self.seat->set_keyboard_focus(nullptr);
    }
}

void Server::configure(std::uint64_t id, std::int32_t width, std::int32_t height) {
    if (Window* w = impl_->window(id); w != nullptr) {
        (void)w->toplevel->configure(width, height);
    }
}

void Server::set_maximized(std::uint64_t id, bool on) {
    if (Window* w = impl_->window(id); w != nullptr) {
        w->toplevel->set_maximized(on);
    }
}

void Server::set_fullscreen(std::uint64_t id, bool on) {
    if (Window* w = impl_->window(id); w != nullptr) {
        w->toplevel->set_fullscreen(on);
    }
}

void Server::close(std::uint64_t id) {
    if (Window* w = impl_->window(id); w != nullptr) {
        w->toplevel->close();
    }
}

rust::String Server::window_title(std::uint64_t id) const {
    const Window* w = impl_->window(id);
    return w != nullptr ? to_rust(w->toplevel->title()) : rust::String();
}

rust::String Server::window_app_id(std::uint64_t id) const {
    const Window* w = impl_->window(id);
    return w != nullptr ? to_rust(w->toplevel->app_id()) : rust::String();
}

Size Server::window_size(std::uint64_t id) const {
    Size size{};
    if (const Window* w = impl_->window(id); w != nullptr) {
        const lm::XdgGeometry g = w->toplevel->geometry();
        lm::Surface& root = w->toplevel->surface();
        const bool has_geometry = g.width > 0 && g.height > 0;
        size.width = has_geometry ? g.width : root.surface_width();
        size.height = has_geometry ? g.height : root.surface_height();
    }
    return size;
}

std::uint32_t Server::bind_key(std::uint32_t mods, std::uint32_t keysym) {
    const auto id = static_cast<std::uint32_t>(impl_->bindings.size() + 1);
    impl_->bindings.push_back({id, mods & kBindableMods, xkb_keysym_to_lower(keysym)});
    return id;
}

std::unique_ptr<Server> server_new(const Config& config, rust::String& error) {
    auto impl = std::make_unique<Server::Impl>();
    if (std::string why = impl->init(config); !why.empty()) {
        error = to_rust(why);
        return nullptr;
    }
    return std::make_unique<Server>(std::move(impl));
}

std::uint32_t keysym_from_name(rust::Str name) {
    const std::string s(name);
    const xkb_keysym_t exact = xkb_keysym_from_name(s.c_str(), XKB_KEYSYM_NO_FLAGS);
    return exact != XKB_KEY_NoSymbol ? exact
                                     : xkb_keysym_from_name(s.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
}

} // namespace lum
