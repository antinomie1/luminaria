// luminaria_rs.cpp — the C++ half of the Rust binding, and the only place the
// modules appear. xmake compiles it (clang + C++ modules); cc never sees it.
//
// This is an ordinary translation unit that imports a module, not a module
// unit, so it has no `module;` line: includes first, then the import, exactly
// like examples/minimal.cpp.
//
// What it is: a compositor's wiring with the policy taken out. What every
// compositor does the same way and the library already does — the frame pump,
// frame callbacks, presentation feedback, the cursor — is the library's
// (`SceneRenderer`, `Cursor`); the rest of that list — globals, popups, layer
// shell, pointer focus, screen capture, the lock screen, input methods — is
// done here. What a window manager decides — where windows go, their stacking,
// sizes, focus, bindings, workspaces — is Rust's, and arrives as plain calls.
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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "luminaria/luminaria_rs.h"
#include "luminaria/src/lib.rs.h"

import luminaria.gpu;
import luminaria.desktop;

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

// Screen capture copies whole rows of these straight into the client's RGBA.
static_assert(sizeof(lm::Pixel) == 4);

Rect to_rect(const lm::Box& b) { return Rect{b.x, b.y, b.width, b.height}; }

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

struct LayerEntry {
    lm::LayerSurface* layer = nullptr; // null once destroyed
    std::uint64_t output = 0;
    bool added = false; // in LayerManager: only from the client's initial commit on
    bool dead = false;
    Conn<lm::LayerSurfaceMap> on_map;
    Conn<lm::LayerSurfaceUnmap> on_unmap;
    Conn<lm::LayerSurfaceDestroy> on_destroy;
    Conn<lm::LayerSurfaceStateChange> on_state;
    Conn<lm::LayerSurfacePopup> on_popup;
};

struct LockSurfaceEntry {
    lm::SessionLockSurface* surface = nullptr;
    bool dead = false;
    Conn<lm::SessionLockSurfaceMap> on_map;
    Conn<lm::SessionLockSurfaceDestroy> on_destroy;
};

/// A touchscreen point, from its down to its up: the surface and window it
/// went down on keep it wherever the finger goes, as the pointer's implicit
/// grab does. `surface` is invalid when it went down on no client.
struct TouchSlot {
    std::int32_t id = 0;
    lm::SurfaceId surface;
    std::uint64_t window = 0;
};

struct OutputEntry {
    std::uint64_t id = 0;
    lm::Output* output = nullptr; // null once unplugged
    bool dead = false;
    lm::SceneOutput state;
    std::optional<lm::OutputGlobal> global;
    lm::Box usable;           // what layer-shell exclusive zones leave, global coordinates
    bool drew_locked = false; // the last frame on screen showed only the lock
    Conn<lm::FrameEvent> on_frame;
    Conn<lm::PresentEvent> on_present;
    Conn<lm::OutputDestroy> on_destroy;
    Conn<lm::OutputModeChange> on_mode;
};

struct Binding {
    std::uint32_t id;
    std::uint32_t mods;
    std::uint32_t keysym;
};

} // namespace detail

using namespace detail;

struct Server::Impl {
    // Declaration order is destruction order, reversed, and it matters:
    //  * `renderer` first, so it dies last: every SceneOutput below caches GPU
    //    textures it owns ("the renderer must outlive the Display").
    //  * `display` before the globals and the backends, which hold its loop.
    //  * anything holding a Seat& (data devices, constraints, text input, the
    //    input method) after `seat`, and data-control after the data devices.
    //  * the per-object state last, so it dies first: its signal connections
    //    are gone before any object they point into.
    lm::SceneRenderer renderer;
    std::optional<lm::Display> display;
    // Before both bare-metal backends, which hold a pointer to it.
    std::optional<lm::Session> session;

    std::optional<lm::Compositor> compositor;
    std::optional<lm::Subcompositor> subcompositor;
    std::optional<lm::XdgShell> shell;
    std::optional<lm::LayerShell> layer_shell;
    std::optional<lm::SinglePixelBufferManager> single_pixel;
    std::optional<lm::Presentation> presentation;
    std::optional<lm::Viewporter> viewporter;
    std::optional<lm::FractionalScaleManager> fractional;
    std::optional<lm::XdgDecorationManager> decoration;
    std::optional<lm::CursorShapeManager> cursor_shape;
    std::optional<lm::FifoManager> fifo;
    std::optional<lm::CommitTimingManager> commit_timing;
    std::optional<lm::TearingControlManager> tearing;
    std::optional<lm::ContentTypeManager> content_type;
    std::optional<lm::BackgroundEffectManager> background_effect;
    std::optional<lm::XdgActivation> activation;
    std::optional<lm::XdgToplevelIconManager> icons;
    std::optional<lm::IdleInhibitManager> idle_inhibit;
    std::optional<lm::IdleNotifier> idle_notify;
    std::optional<lm::LinuxDmabuf> dmabuf;
    std::optional<lm::DrmSyncobjManager> syncobj;
    std::optional<lm::ScreencopyManager> screencopy;
    std::optional<lm::ForeignToplevelManager> foreign;
    std::optional<lm::WorkspaceManager> workspaces;
    std::optional<lm::OutputManager> output_manager;
    std::optional<lm::SessionLockManager> locks;
    std::optional<lm::Seat> seat;
    std::optional<lm::DataDeviceManager> data_device;
    std::optional<lm::PrimarySelectionManager> primary_selection;
    std::optional<lm::DataControlManager> data_control;
    std::optional<lm::PointerConstraints> constraints;
    std::optional<lm::RelativePointerManager> relative;
    std::optional<lm::TextInputManager> text_input;
    std::optional<lm::InputMethodManager> input_method;

    std::unique_ptr<lm::Backend> backend;
    lm::WaylandBackend* nested = nullptr; // == backend.get() when nested; input comes from it
    std::optional<lm::LibinputBackend> input; // bare metal only

    std::string socket;
    lm::OutputLayout layout;
    lm::PopupTree popup_tree;
    lm::LayerManager layers;
    std::optional<lm::KeymapState> keymap; // nested: built from the host's keymap
    lm::KeyRouter keys;
    // The pointer and its picture. After the renderer, the layout, the seat,
    // cursor-shape and screencopy, all of which it borrows.
    std::optional<lm::Cursor> cursor;
    lm::Color background{0.1f, 0.1f, 0.12f, 1.0f};

    std::vector<Item> items;            // Rust's list, global logical coordinates
    std::vector<lm::SceneItem> scene;   // `items` resolved, plus layers and popups (global)
    std::vector<lm::SceneItem> local;   // `scene` in one output's coordinates
    std::vector<lm::SurfaceAt> tree;    // scratch for surface_tree()
    std::deque<Event> events;
    std::vector<Binding> bindings;

    std::uint64_t next_window = 1;
    std::uint64_t next_output = 1;
    std::uint64_t focused = 0;    // the window Rust focused
    lm::SurfaceId layer_focus;    // an on-demand layer surface the user clicked into
    std::uint64_t ptr_window = 0; // the window under the pointer
    bool wm_grab = false;         // Rust is dragging something: clients see no motion
    std::vector<std::uint32_t> held; // keys a client was told are down
    std::vector<TouchSlot> touch_slots; // touch points that are down
    std::uint32_t workspace_group = 0;
    bool applying_outputs = false; // inside an output-management apply: one update at the end

    // The lock screen. `lock_pending`: a lock was asked for and the screens
    // are already blanked, but not every output has shown that yet, so the
    // client has not been told it is locked. `lock_failed`: a lock client died
    // without unlocking, and the session stays locked — fails closed — until
    // another lock client unlocks it.
    bool lock_pending = false;
    bool lock_failed = false;
    bool reported_locked = false;

    Conn<lm::NewOutput> on_new_output;
    Conn<lm::NewToplevel> on_new_toplevel;
    Conn<lm::NewPopup> on_new_popup;
    Conn<lm::NewLayerSurface> on_new_layer;
    Conn<lm::PointerMotionAbsEvent> on_motion;
    Conn<lm::PointerMotionEvent> on_rel_motion;
    Conn<lm::PointerButtonEvent> on_button;
    Conn<lm::PointerAxisEvent> on_axis;
    Conn<lm::KeyEvent> on_key;
    Conn<lm::ModifiersEvent> on_mods;
    Conn<lm::KeymapChange> on_keymap;
    Conn<lm::HostCloseRequest> on_close;
    Conn<lm::InputCapabilities> on_caps;
    Conn<lm::TouchDownEvent> on_touch_down;
    Conn<lm::TouchMotionEvent> on_touch_motion;
    Conn<lm::TouchUpEvent> on_touch_up;
    Conn<lm::TouchFrameEvent> on_touch_frame;
    Conn<lm::TouchCancelEvent> on_touch_cancel;
    Conn<lm::SessionActive> on_session;
    Conn<lm::ForeignToplevelRequest> on_foreign;
    Conn<lm::ActivationRequest> on_activate;
    Conn<lm::WorkspaceRequest> on_workspace;
    Conn<lm::OutputConfigRequest> on_output_config;
    Conn<lm::IdleInhibitChange> on_inhibit;
    Conn<lm::NewSessionLock> on_new_lock;
    Conn<lm::NewSessionLockSurface> on_lock_surface;
    Conn<lm::SessionLockDestroy> on_lock_destroy;

    std::vector<std::unique_ptr<LockSurfaceEntry>> lock_surfaces;
    std::vector<std::unique_ptr<OutputEntry>> outputs;
    std::vector<std::unique_ptr<LayerEntry>> layer_entries;
    std::vector<std::unique_ptr<PopupEntry>> popups;
    std::vector<std::unique_ptr<Window>> windows;

    // ------------------------------------------------------------- lookups

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

    Window* window_of(const lm::Toplevel& toplevel) const {
        for (const auto& w : windows) {
            if (w->toplevel == &toplevel) {
                return w.get();
            }
        }
        return nullptr;
    }

    Window* window_of(const lm::Surface& surface) const {
        for (const auto& w : windows) {
            if (w->toplevel != nullptr && &w->toplevel->surface() == &surface) {
                return w.get();
            }
        }
        return nullptr;
    }

    OutputEntry* output(std::uint64_t id) const {
        for (const auto& o : outputs) {
            if (o->id == id && o->output != nullptr) {
                return o.get();
            }
        }
        return nullptr;
    }

    OutputEntry* entry_of(const lm::Output* output) const {
        for (const auto& o : outputs) {
            if (output != nullptr && o->output == output) {
                return o.get();
            }
        }
        return nullptr;
    }

    /// The output a client means by one of its wl_output resources.
    OutputEntry* output_of(wl_resource* resource) const {
        if (resource == nullptr) {
            return nullptr;
        }
        wl_client* client = wl_resource_get_client(resource);
        for (const auto& o : outputs) {
            if (o->output != nullptr && o->global.has_value() &&
                o->global->resource_for(client) == resource) {
                return o.get();
            }
        }
        return nullptr;
    }

    const lm::Pointer& pointer() const { return cursor->pointer(); }

    /// "Compositor's choice": the output under the pointer, else the first.
    OutputEntry* default_output() const {
        if (pointer().present()) {
            if (lm::Output* on = layout.at(static_cast<int>(pointer().x()),
                                           static_cast<int>(pointer().y()));
                on != nullptr) {
                for (const auto& o : outputs) {
                    if (o->output == on) {
                        return o.get();
                    }
                }
            }
        }
        for (const auto& o : outputs) {
            if (o->output != nullptr) {
                return o.get();
            }
        }
        return nullptr;
    }

    bool locked() const { return lock_pending || lock_failed || locks->session_locked(); }

    /// Nothing reports damage for a layout change, a popup or the cursor; ask
    /// every output for a frame and let the scene diff work out what moved.
    void schedule_all() {
        for (const auto& o : outputs) {
            if (o->output != nullptr) {
                o->output->schedule_frame();
            }
        }
    }

    // ------------------------------------------------------------- scene

    void append_window(const Item& it) {
        const Window* w = window(it.window);
        if (w == nullptr || !w->mapped) {
            return;
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
                         .tag = it.window,
                         // Blur only what the client asked to have blurred
                         // (ext-background-effect). A window that declared
                         // nothing costs nothing; the CPU path ignores it.
                         .blur = {.enabled = true, .surface_regions = true}});
    }

    void append_layers(lm::Layer layer) {
        for (const auto& o : outputs) {
            if (o->output != nullptr) {
                layers.append_to(scene, static_cast<std::uint32_t>(o->id), layer);
            }
        }
    }

    /// The input method's candidate window, under the text cursor of whatever
    /// text field is focused.
    void append_input_popups() {
        lm::InputMethod* im = input_method->current();
        lm::TextInput* ti = text_input->focused();
        lm::Surface* field = ti != nullptr ? ti->focused_surface() : nullptr;
        double fx = 0, fy = 0;
        if (im == nullptr || field == nullptr || !surface_origin(field->id(), fx, fy)) {
            return;
        }
        for (lm::InputPopupSurface* popup : im->popup_surfaces()) {
            lm::Surface& s = popup->surface();
            const lm::Box& r = popup->text_input_rectangle();
            const int x = static_cast<int>(fx) + r.x;
            const int y = static_cast<int>(fy) + r.y + r.height;
            scene.push_back({.kind = lm::SceneItem::Kind::surface,
                             .box = {x, y, s.surface_width(), s.surface_height()},
                             .surface = s.id(),
                             .x = x,
                             .y = y});
        }
    }

    /// The lock screen replaces everything: each output shows its own lock
    /// surface, or nothing at all — never a window.
    void build_lock_scene() {
        lm::SessionLock* lock = locks->current();
        if (lock == nullptr) {
            return; // the lock client died: black, and still locked
        }
        for (lm::SessionLockSurface* s : lock->surfaces()) {
            OutputEntry* o = output_of(s->output_resource());
            if (o == nullptr || !s->mapped()) {
                continue;
            }
            const lm::Box box = layout.box_of(*o->output);
            scene.push_back({.kind = lm::SceneItem::Kind::surface,
                             .box = box,
                             .surface = s->surface().id(),
                             .x = box.x,
                             .y = box.y});
        }
    }

    /// Everything, back to front, in global coordinates: background and
    /// bottom layers, Rust's list, top and overlay layers, then popups (window
    /// popups, panel menus) and the input method's popup above it all.
    void build_scene() {
        scene.clear();
        if (locked()) {
            build_lock_scene();
            return;
        }
        append_layers(lm::Layer::background);
        append_layers(lm::Layer::bottom);
        for (const Item& it : items) {
            switch (it.kind) {
            case ItemKind::Window:
                append_window(it);
                break;
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
        append_layers(lm::Layer::top);
        append_layers(lm::Layer::overlay);
        popup_tree.append_to(scene);
        append_input_popups();
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

    // ------------------------------------------------------------- focus

    /// Who gets the keyboard, highest claim first: the lock screen; a top or
    /// overlay layer surface that demands it exclusively (a launcher, a
    /// password prompt); an on-demand layer surface the user clicked into;
    /// the window Rust focused.
    void apply_keyboard_focus() {
        lm::Surface* target = nullptr;
        if (locked()) {
            if (lm::SessionLock* lock = locks->current(); lock != nullptr) {
                OutputEntry* here = default_output();
                for (lm::SessionLockSurface* s : lock->surfaces()) {
                    if (!s->mapped()) {
                        continue;
                    }
                    if (target == nullptr || output_of(s->output_resource()) == here) {
                        target = &s->surface();
                    }
                }
            }
        } else if (lm::LayerSurface* ex = exclusive_layer(); ex != nullptr) {
            target = &ex->surface();
        } else if (lm::Surface* clicked = lm::surface_from_id(layer_focus); clicked != nullptr) {
            target = clicked;
        } else if (Window* w = window(focused); w != nullptr && w->mapped) {
            target = &w->toplevel->surface();
        }
        const lm::SurfaceId want = target != nullptr ? target->id() : lm::SurfaceId{};
        if (want != seat->keyboard_focus()) {
            seat->set_keyboard_focus(target);
        }
    }

    lm::LayerSurface* exclusive_layer() const {
        lm::LayerSurface* best = nullptr;
        for (const auto& e : layer_entries) {
            lm::LayerSurface* ls = e->layer;
            if (ls == nullptr || !ls->mapped() ||
                ls->keyboard_interactivity() != lm::LayerKeyboardInteractivity::exclusive ||
                ls->layer() < lm::Layer::top) {
                continue;
            }
            if (best == nullptr || ls->layer() >= best->layer()) {
                best = ls;
            }
        }
        return best;
    }

    /// Hand the pointer's constraint (a game's pointer lock, a confined
    /// drawing app) to the surface under it, and take it away on leave.
    void apply_pointer_constraint() {
        lm::Surface* focus = lm::surface_from_id(seat->pointer_focus());
        lm::PointerConstraint* want = focus != nullptr ? constraints->constraint_for(*focus) : nullptr;
        lm::PointerConstraint* current = constraints->active_constraint();
        if (current != nullptr && current != want) {
            current->deactivate();
        }
        if (want != nullptr && !want->active()) {
            want->activate();
        }
    }

    /// Send pointer enter/motion for the current position.
    ///
    /// Three cases. A drag-and-drop is in progress: hit-test as usual, and
    /// the seat routes it to the drag. A button is held (the seat's implicit
    /// grab): every event belongs to the surface that took the press, in its
    /// own coordinates, wherever the pointer is. Otherwise: whatever is under
    /// the pointer.
    void update_pointer_focus() {
        if (!pointer().present()) {
            return;
        }
        build_scene();
        const lm::SceneHit hit = lm::scene_hit_test(scene, pointer().x(), pointer().y());
        ptr_window = hit.hit ? hit.tag : 0;
        if (wm_grab) {
            return; // the window manager is moving or resizing something
        }
        if (const lm::SurfaceId grab = seat->pointer_grab(); grab.valid() && !seat->dragging()) {
            double ox = 0, oy = 0;
            if (lm::surface_from_id(grab) != nullptr && surface_origin(grab, ox, oy)) {
                seat->pointer_motion(pointer().x() - ox, pointer().y() - oy);
            }
            return;
        }
        if (hit.surface == nullptr) {
            if (seat->pointer_focus().valid()) {
                seat->pointer_clear_focus();
            }
        } else {
            seat->pointer_enter(*hit.surface, hit.sx, hit.sy);
        }
        apply_pointer_constraint();
    }

    // ------------------------------------------------------------- frames

    void draw(OutputEntry& o) {
        if (o.output == nullptr) {
            return;
        }
        build_scene();
        const lm::Box view = layout.box_of(*o.output);
        local.clear();
        for (lm::SceneItem item : scene) {
            item.box.x -= view.x;
            item.box.y -= view.y;
            item.x -= view.x;
            item.y -= view.y;
            local.push_back(item);
        }

        // Frame callbacks, presentation feedback and the cursor are the
        // renderer's; what is left here is the lock screen's own bookkeeping.
        const bool showing_lock = locked();
        const lm::Color bg = showing_lock ? lm::Color{0.0f, 0.0f, 0.0f, 1.0f} : background;
        auto outcome = renderer.present(o.state, *o.output, local, bg);
        if (!outcome) {
            warn("present", outcome.error());
            return;
        }
        o.drew_locked = showing_lock;
        if (*outcome == lm::SceneOutcome::unchanged) {
            confirm_lock(); // the screen already shows what this frame would have
        }
    }

    /// ext-session-lock: `locked` may only be sent once no output shows
    /// anything but the lock — so only after each has presented such a frame.
    void confirm_lock() {
        if (!lock_pending) {
            return;
        }
        for (const auto& o : outputs) {
            if (o->output != nullptr && !o->drew_locked) {
                return;
            }
        }
        lock_pending = false;
        if (lm::SessionLock* lock = locks->current(); lock != nullptr) {
            lock->send_locked();
        }
    }

    /// Free everything whose object died during the last dispatch. Here and
    /// not in the destroy slots: a slot must not destroy the state it runs in.
    void reap() {
        std::erase_if(windows, [](const auto& w) { return w->dead; });
        std::erase_if(popups, [](const auto& p) { return p->dead; });
        std::erase_if(layer_entries, [](const auto& l) { return l->dead; });
        std::erase_if(lock_surfaces, [](const auto& l) { return l->dead; });
        std::erase_if(outputs, [](const auto& o) { return o->dead; });
    }

    // ------------------------------------------------------------- windows

    void watch_toplevel(lm::Toplevel& toplevel) {
        auto owned = std::make_unique<Window>();
        Window* w = owned.get();
        w->id = next_window++;
        w->toplevel = &toplevel;
        w->on_map = toplevel.map.connect([this, w](lm::ToplevelMap&) {
            w->mapped = true;
            // Render at the output's density, both the integer hint and the
            // exact scale for clients that speak fractional-scale.
            if (OutputEntry* o = default_output(); o != nullptr) {
                lm::Surface& surface = w->toplevel->surface();
                surface.set_preferred_buffer_scale(o->output->scale());
                surface.set_preferred_buffer_transform(o->output->transform());
                fractional->set_scale(surface, o->output->scale() * 120);
            }
            push(EventKind::WindowMap, w->id);
            schedule_all();
        });
        w->on_unmap = toplevel.unmap.connect([this, w](lm::ToplevelUnmap&) {
            w->mapped = false;
            if (focused == w->id) {
                focused = 0;
                apply_keyboard_focus();
            }
            push(EventKind::WindowUnmap, w->id);
            schedule_all();
        });
        w->on_destroy = toplevel.destroy.connect([this, w](lm::ToplevelDestroy&) {
            w->mapped = false;
            w->toplevel = nullptr;
            w->dead = true;
            if (focused == w->id) {
                focused = 0;
                apply_keyboard_focus();
            }
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
        w->on_minimize =
            toplevel.request_minimize.connect([this, w](lm::ToplevelRequestMinimize&) {
                push(EventKind::RequestMinimize, w->id);
                events.back().on = true;
            });
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

    /// Requests from a taskbar (foreign-toplevel) or an activation token are
    /// the same requests the window itself can make, so they reach Rust as
    /// the same events. Closing needs no decision and is done here.
    void foreign_request(lm::ForeignToplevelRequest& r) {
        Window* w = window_of(r.toplevel);
        if (w == nullptr) {
            return;
        }
        using K = lm::ForeignToplevelRequest::Kind;
        switch (r.kind) {
        case K::maximize:
        case K::unmaximize:
            push(EventKind::RequestMaximize, w->id);
            events.back().on = r.kind == K::maximize;
            break;
        case K::fullscreen:
        case K::unfullscreen:
            push(EventKind::RequestFullscreen, w->id);
            events.back().on = r.kind == K::fullscreen;
            break;
        case K::minimize:
        case K::unminimize:
            push(EventKind::RequestMinimize, w->id);
            events.back().on = r.kind == K::minimize;
            break;
        case K::activate:
            push(EventKind::RequestActivate, w->id);
            break;
        case K::close:
            r.toplevel.close();
            break;
        }
    }

    // ------------------------------------------------------------- layer shell

    void watch_layer(lm::LayerSurface& ls) {
        OutputEntry* o = output_of(ls.output_resource());
        if (o == nullptr) {
            o = default_output();
        }
        if (o == nullptr) {
            ls.close(); // nowhere to put it
            return;
        }
        auto owned = std::make_unique<LayerEntry>();
        LayerEntry* e = owned.get();
        e->layer = &ls;
        e->output = o->id;
        const auto changed = [this, e] {
            if (OutputEntry* out = output(e->output); out != nullptr) {
                arrange(*out);
            }
            apply_keyboard_focus();
            schedule_all();
        };
        e->on_map = ls.map.connect([this, e, changed](lm::LayerSurfaceMap&) {
            layers.set_mapped(*e->layer, true);
            changed();
        });
        e->on_unmap = ls.unmap.connect([this, e, changed](lm::LayerSurfaceUnmap&) {
            layers.set_mapped(*e->layer, false);
            if (layer_focus == e->layer->surface().id()) {
                layer_focus = {};
            }
            changed();
        });
        // The first of these is the client's initial commit, and only from
        // then on may it be configured: an earlier arrange() would configure
        // it before it said what it wants.
        e->on_state = ls.state_change.connect([this, e, changed](lm::LayerSurfaceStateChange&) {
            if (!e->added) {
                layers.add(*e->layer, static_cast<std::uint32_t>(e->output));
                e->added = true;
            }
            changed();
        });
        e->on_destroy = ls.destroy.connect([this, e, changed](lm::LayerSurfaceDestroy&) {
            layers.remove(*e->layer);
            if (layer_focus == e->layer->surface().id()) {
                layer_focus = {};
            }
            e->layer = nullptr;
            e->dead = true;
            changed();
        });
        // A panel's own menu: positioned against the panel, not a window.
        e->on_popup = ls.new_popup.connect([this](lm::LayerSurfacePopup& p) {
            popup_tree.set_parent(p.popup, p.layer_surface.surface().id());
        });
        layer_entries.push_back(std::move(owned));
    }

    /// Lay out one output's layer surfaces, configure them, and tell Rust if
    /// the area left for windows changed.
    void arrange(OutputEntry& o) {
        const lm::Box full = layout.box_of(*o.output);
        const lm::Box usable = layers.arrange(static_cast<std::uint32_t>(o.id), full, full);
        if (usable.x != o.usable.x || usable.y != o.usable.y || usable.width != o.usable.width ||
            usable.height != o.usable.height) {
            o.usable = usable;
            push(EventKind::OutputChange);
            events.back().output = o.id;
            // How big a window may get, for maximize and configure_bounds.
            shell->set_bounds(usable.width, usable.height);
        }
    }

    // ------------------------------------------------------------- outputs

    void watch_output(lm::Output& output) {
        auto owned = std::make_unique<OutputEntry>();
        OutputEntry* o = owned.get();
        o->id = next_output++;
        o->output = &output;
        layout.add_auto(output);
        const lm::Box box = layout.box_of(output);
        o->usable = box;

        if (auto s = renderer.reset(o->state, output, DRM_FORMAT_XRGB8888); !s) {
            warn("render target (falling back to CPU)", s.error());
        }
        auto global = lm::OutputGlobal::create(*display, output.width(), output.height(),
                                               "luminaria-" + std::to_string(o->id));
        if (global) {
            o->global.emplace(std::move(*global));
            sync_global(*o);
            // Capture registers per binding: the client names the output by
            // its own wl_output resource. The callback looks the output up by
            // id when it runs, because it outlives this entry.
            const std::uint64_t id = o->id;
            o->global->on_bind([this, id](wl_resource* resource) {
                if (OutputEntry* out = this->output(id); out != nullptr) {
                    screencopy->add_output(resource, out->output->width(), out->output->height(),
                                           [this, id](int x, int y, int w, int h,
                                                      std::vector<std::uint8_t>& rgba) {
                                               return capture(id, x, y, w, h, rgba);
                                           });
                }
            });
            workspaces->group_add_output(workspace_group, *o->global);
            workspaces->done();
        } else {
            warn("wl_output", global.error());
        }
        shell->set_bounds(box.width, box.height);
        // The same name as the wl_output, which is what kanshi matches on.
        output_manager->add_head(output, {.name = "luminaria-" + std::to_string(o->id),
                                          .description = backend_description()});

        o->on_frame = output.frame.connect([this, o](lm::FrameEvent&) { draw(*o); });
        o->on_present =
            output.present.connect([this](lm::PresentEvent&) { confirm_lock(); });
        o->on_mode = output.mode_changed.connect([this, o](lm::OutputModeChange&) {
            // Everything sized for the old mode has to be rebuilt.
            if (auto s = renderer.reset(o->state, *o->output, DRM_FORMAT_XRGB8888); !s) {
                warn("render target (falling back to CPU)", s.error());
            }
            relayout(*o);
        });
        o->on_destroy = output.destroy.connect([this, o](lm::OutputDestroy&) {
            layers.close_output(static_cast<std::uint32_t>(o->id));
            layout.remove(*o->output);
            // The render state goes now, while the Output it was built for
            // still exists; the entry itself waits for reap().
            o->state = lm::SceneOutput{};
            drop_global(*o);
            o->output = nullptr;
            o->dead = true;
            push(EventKind::OutputDestroy);
            events.back().output = o->id;
            confirm_lock(); // one fewer screen that has to show the lock
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

        // libinput only reports deltas, so the pointer needs somewhere to
        // start; the middle of the first screen is where users look for it.
        if (input.has_value() && !pointer().present()) {
            cursor->move_to(box.x + box.width / 2.0, box.y + box.height / 2.0);
        }
        if (locked()) {
            reconfigure_lock_surfaces();
        }
        output.schedule_frame();
    }

    /// Mirror the output's current state into its wl_output.
    void sync_global(OutputEntry& o) {
        if (!o.global.has_value()) {
            return;
        }
        lm::Output& out = *o.output;
        const lm::Box box = layout.box_of(out);
        o.global->set_logical_position(box.x, box.y);
        if (const auto modes = out.modes(); !modes.empty()) {
            o.global->set_modes(modes); // a real monitor: tell clients what it can do
        }
        o.global->set_mode(out.width(), out.height(), out.current_mode().refresh_mhz);
        o.global->set_scale(out.scale());
        o.global->set_transform(out.transform());
    }

    void drop_global(OutputEntry& o) {
        if (o.global.has_value()) {
            workspaces->group_remove_output(workspace_group, *o.global);
            workspaces->done();
            o.global.reset();
        }
    }

    /// The output changed size, scale, transform or position.
    void relayout(OutputEntry& o) {
        const lm::Box old = layout.box_of(*o.output);
        layout.add(*o.output, old.x, old.y); // re-adding re-reads its logical size
        sync_global(o);
        o.usable = {};                       // force the OutputChange below
        arrange(o);
        // Clamped back into the new layout, and redrawn either way: the same
        // global position is somewhere else on this output now.
        if (!pointer().present() || !cursor->move_to(pointer().x(), pointer().y())) {
            cursor->refresh();
        }
        if (locked()) {
            reconfigure_lock_surfaces();
        }
        if (!applying_outputs) {
            output_manager->update(); // wlr-randr and kanshi see it too
        }
        o.output->schedule_frame();
    }

    std::string backend_description() const {
        if (nested != nullptr) {
            return "luminaria nested output";
        }
        return input.has_value() ? "luminaria DRM output" : "luminaria headless output";
    }

    // The three set_output_* below are both Rust's and wlr-output-management's.

    void set_scale(OutputEntry& o, int scale) {
        o.output->set_scale(scale);
        if (auto s = renderer.reset(o.state, *o.output, DRM_FORMAT_XRGB8888); !s) {
            warn("render target (falling back to CPU)", s.error());
        }
        relayout(o);
    }

    void set_transform(OutputEntry& o, lm::Transform transform) {
        o.output->set_transform(transform);
        if (auto s = renderer.reset(o.state, *o.output, DRM_FORMAT_XRGB8888); !s) {
            warn("render target (falling back to CPU)", s.error());
        }
        relayout(o);
    }

    void set_position(OutputEntry& o, int x, int y) {
        layout.add(*o.output, x, y);
        relayout(o);
    }

    /// Whether `output` can be driven at `mode`: one it lists, or — for an
    /// output with no list (nested, headless) — the size it already has.
    static bool has_mode(const lm::Output& output, const lm::OutputMode& mode) {
        const auto fits = [&](const lm::OutputMode& m) {
            return m.width == mode.width && m.height == mode.height &&
                   (mode.refresh_mhz == 0 || m.refresh_mhz == 0 ||
                    m.refresh_mhz == mode.refresh_mhz);
        };
        const auto modes = output.modes();
        return modes.empty() ? fits(output.current_mode()) : std::ranges::any_of(modes, fits);
    }

    /// wlr-output-management (wlr-randr, kanshi). Mechanism, not policy: a
    /// configuration this machine can drive is applied as asked, through the
    /// same paths as Rust's set_output_*, and Rust hears OutputChange as usual.
    /// What cannot be done here is refused before anything changes: switching
    /// a head off (an output only leaves the layout by being unplugged),
    /// adaptive sync, a mode the output does not list. A fractional scale is
    /// rounded, which the protocol allows.
    void configure_outputs(lm::OutputConfigRequest& r) {
        for (const lm::OutputHeadConfig& h : r.heads) {
            if (entry_of(h.output) == nullptr || !h.enabled || h.adaptive_sync.value_or(false) ||
                (h.mode && !has_mode(*h.output, *h.mode)) ||
                (h.scale && std::lround(*h.scale) < 1)) {
                return; // `failed`
            }
        }
        if (r.test) {
            r.verdict = lm::OutputConfigVerdict::succeeded;
            return;
        }
        applying_outputs = true;
        // Modes first: they are the only part a backend can still refuse, and
        // a refusal undoes the ones that already switched.
        std::vector<std::pair<lm::Output*, lm::OutputMode>> switched;
        for (const lm::OutputHeadConfig& h : r.heads) {
            if (!h.mode) {
                continue;
            }
            const lm::OutputMode before = h.output->current_mode();
            if (auto s = h.output->set_mode(h.mode->width, h.mode->height, h.mode->refresh_mhz);
                !s) {
                warn("wlr-output-management mode", s.error());
                for (auto it = switched.rbegin(); it != switched.rend(); ++it) {
                    (void)it->first->set_mode(it->second.width, it->second.height,
                                              it->second.refresh_mhz);
                }
                applying_outputs = false;
                output_manager->update();
                return;
            }
            switched.emplace_back(h.output, before);
        }
        for (const lm::OutputHeadConfig& h : r.heads) {
            OutputEntry& o = *entry_of(h.output);
            if (h.scale && std::lround(*h.scale) != o.output->scale()) {
                set_scale(o, static_cast<int>(std::lround(*h.scale)));
            }
            if (h.transform && *h.transform != o.output->transform()) {
                set_transform(o, *h.transform);
            }
            if (h.position) {
                set_position(o, h.position->x, h.position->y);
            }
        }
        applying_outputs = false;
        output_manager->update();
        r.verdict = lm::OutputConfigVerdict::succeeded;
    }

    bool capture(std::uint64_t id, int x, int y, int w, int h, std::vector<std::uint8_t>& rgba) {
        OutputEntry* o = output(id);
        if (o == nullptr) {
            return false;
        }
        auto shown = renderer.read_back(o->state);
        const int ow = o->output->width();
        const int oh = o->output->height();
        // The rectangle is the client's; the frame is ours. Check both before
        // a single byte is copied.
        if (!shown || x < 0 || y < 0 || w <= 0 || h <= 0 || x > ow - w || y > oh - h ||
            shown->size() < static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh)) {
            return false;
        }
        rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        for (int row = 0; row < h; ++row) {
            const lm::Pixel* src = shown->data() + static_cast<std::size_t>(y + row) * ow + x;
            std::memcpy(rgba.data() + static_cast<std::size_t>(row) * w * 4, src,
                        static_cast<std::size_t>(w) * 4);
        }
        return true;
    }

    /// For ext-image-copy-capture cursor sessions: the pointer as it looks on
    /// `resource`'s output, if it is there.
    bool capture_cursor(wl_resource* resource, lm::CursorCapture& out) {
        const OutputEntry* o = output_of(resource);
        return o != nullptr && !locked() && cursor->capture(*o->output, out);
    }

    // ------------------------------------------------------------- lock screen

    void watch_lock(lm::SessionLock& lock) {
        lock_pending = true; // blank every screen now; confirm once they show it
        on_lock_surface = lock.new_surface.connect([this](lm::NewSessionLockSurface& e) {
            auto owned = std::make_unique<LockSurfaceEntry>();
            LockSurfaceEntry* entry = owned.get();
            entry->surface = &e.lock_surface;
            entry->on_map = e.lock_surface.map.connect([this](lm::SessionLockSurfaceMap&) {
                apply_keyboard_focus();
                schedule_all();
            });
            entry->on_destroy = e.lock_surface.destroy.connect(
                [this, entry](lm::SessionLockSurfaceDestroy&) {
                    entry->surface = nullptr;
                    entry->dead = true;
                    apply_keyboard_focus();
                    schedule_all();
                });
            lock_surfaces.push_back(std::move(owned));
            configure_lock_surface(e.lock_surface);
        });
        on_lock_destroy = lock.destroy.connect([this](lm::SessionLockDestroy& e) {
            lock_pending = false;
            lock_failed = !e.unlocked; // a crash is not an unlock
            report_lock();
            apply_keyboard_focus();
            schedule_all();
        });
        report_lock();
        // Nothing of the session may stay under the pointer or the keyboard.
        seat->pointer_clear_focus();
        apply_keyboard_focus();
        schedule_all();
        confirm_lock(); // no outputs at all: nothing to wait for
    }

    void configure_lock_surface(lm::SessionLockSurface& s) {
        if (OutputEntry* o = output_of(s.output_resource()); o != nullptr) {
            const lm::Box box = layout.box_of(*o->output);
            (void)s.configure(static_cast<std::uint32_t>(box.width),
                              static_cast<std::uint32_t>(box.height));
        }
    }

    void reconfigure_lock_surfaces() {
        for (const auto& e : lock_surfaces) {
            if (e->surface != nullptr) {
                configure_lock_surface(*e->surface);
            }
        }
    }

    void report_lock() {
        const bool now = locked();
        if (now != reported_locked) {
            reported_locked = now;
            push(EventKind::SessionLock);
            events.back().on = now;
        }
    }

    // ------------------------------------------------------------- input
    //
    // Two sources, one path: the nested backend reports absolute positions
    // inside the host window, libinput reports deltas. Both end in
    // pointer_moved(), and buttons, axes and keys are the same events.

    /// The pointer is somewhere new (`Cursor` has already redrawn it): tell
    /// the client under it.
    void pointer_moved() {
        idle_notify->notify_activity();
        update_pointer_focus();
        Event ev{};
        ev.kind = EventKind::PointerMotion;
        ev.window = ptr_window;
        ev.x = pointer().x();
        ev.y = pointer().y();
        events.push_back(ev);
    }

    void pointer_left() {
        cursor->leave();
        ptr_window = 0;
        seat->pointer_clear_focus();
    }

    /// A click or a touch went down on `target`: what that means for menus
    /// and the keyboard, whichever device it came from.
    void pressed_on(lm::SurfaceId target) {
        if (locked()) {
            return;
        }
        (void)popup_tree.dismiss_grabs_outside(target); // menu semantics
        // Clicking into an on-demand layer surface (a panel's search box)
        // gives it the keyboard; clicking anywhere else takes it back.
        lm::LayerSurface* ls = layers.keyboard_target(target);
        const lm::SurfaceId next = ls != nullptr ? ls->surface().id() : lm::SurfaceId{};
        if (next != layer_focus) {
            layer_focus = next;
            apply_keyboard_focus();
        }
    }

    void button(std::uint32_t code, bool pressed) {
        idle_notify->notify_activity();
        if (pressed) {
            pressed_on(seat->pointer_focus());
        }
        Event ev{};
        ev.kind = EventKind::PointerButton;
        ev.window = locked() ? 0 : ptr_window;
        ev.code = code;
        ev.on = pressed;
        ev.x = pointer().x();
        ev.y = pointer().y();
        events.push_back(ev);
        if (seat->pointer_focus().valid()) {
            seat->pointer_button(code, pressed);
        }
        if (!pressed) {
            update_pointer_focus(); // an implicit grab may have ended; re-pick
        }
    }

    void axis(const lm::PointerAxisEvent& e) {
        idle_notify->notify_activity();
        if (wm_grab) {
            return;
        }
        // Notches win over the smooth deltas that accompany them.
        if (e.dx_steps != 0 || e.dy_steps != 0) {
            seat->pointer_axis_discrete(e.dx_steps, e.dy_steps);
        } else if (e.dx != 0.0 || e.dy != 0.0) {
            seat->pointer_axis(e.dx, e.dy);
        }
        if (e.stop_horizontal || e.stop_vertical) {
            seat->pointer_axis_stop(e.stop_horizontal, e.stop_vertical);
        }
    }

    void relative_motion(double dx, double dy) {
        // Games under a pointer lock read these; the pointer itself may not move.
        lm::CursorManager::send_relative_motion(&*relative, dx, dy);
        const lm::PointerConstraint* active = constraints->active_constraint();
        if (active != nullptr && active->type() == lm::PointerConstraintType::locked) {
            idle_notify->notify_activity();
            return;
        }
        build_scene();
        const lm::Box bounds =
            lm::CursorManager::pointer_bounds(&*constraints, scene, layout.bounds());
        if (cursor->move_by(dx, dy, bounds)) {
            pointer_moved();
        }
    }

    // ------------------------------------------------------------- touch

    /// Where a touchscreen point lands, in global logical coordinates. The
    /// panel is taken to cover the first output — the built-in screen on a
    /// laptop or tablet — and to share its native orientation, so a rotated
    /// output rotates touch with it. libinput gives the point in units of the
    /// panel; false when there is no output to put it on.
    bool touch_to_global(double u, double v, double& x, double& y) const {
        const OutputEntry* o = nullptr;
        for (const auto& entry : outputs) {
            if (entry->output != nullptr) {
                o = entry.get();
                break;
            }
        }
        if (o == nullptr) {
            return false;
        }
        // A badly calibrated panel can report a little past its edges; [0, 1)
        // keeps the point on this output rather than on its neighbour.
        const double below_one = std::nextafter(1.0, 0.0);
        const lm::UnitPoint p = lm::transform_unit_point(
            o->output->transform(), {std::clamp(u, 0.0, below_one), std::clamp(v, 0.0, below_one)});
        const lm::Box box = layout.box_of(*o->output);
        x = box.x + p.x * box.width;
        y = box.y + p.y * box.height;
        return true;
    }

    TouchSlot* touch_slot(std::int32_t id) {
        for (TouchSlot& t : touch_slots) {
            if (t.id == id) {
                return &t;
            }
        }
        return nullptr;
    }

    void push_touch(EventKind kind, const TouchSlot& t, double x, double y) {
        Event ev{};
        ev.kind = kind;
        ev.window = t.window;
        ev.code = static_cast<std::uint32_t>(t.id);
        ev.x = x;
        ev.y = y;
        events.push_back(ev);
    }

    /// Hit-tested with the same scene the pointer uses, so a finger and a
    /// click on the same pixel reach the same surface.
    void touch_down(const lm::TouchDownEvent& e) {
        idle_notify->notify_activity();
        double x = 0, y = 0;
        if (!touch_to_global(e.x, e.y, x, y)) {
            return;
        }
        build_scene();
        const lm::SceneHit hit = lm::scene_hit_test(scene, x, y);
        pressed_on(hit.surface != nullptr ? hit.surface->id() : lm::SurfaceId{});
        std::erase_if(touch_slots, [&](const TouchSlot& t) { return t.id == e.id; });
        TouchSlot slot{e.id, {}, locked() || !hit.hit ? 0 : hit.tag};
        if (hit.surface != nullptr) {
            slot.surface = hit.surface->id();
            seat->touch_down(*hit.surface, e.id, hit.sx, hit.sy);
        }
        touch_slots.push_back(slot);
        push_touch(EventKind::TouchDown, slot, x, y);
    }

    void touch_motion(const lm::TouchMotionEvent& e) {
        idle_notify->notify_activity();
        const TouchSlot* slot = touch_slot(e.id);
        double x = 0, y = 0;
        if (slot == nullptr || !touch_to_global(e.x, e.y, x, y)) {
            return;
        }
        // The surface's origin now, not at the down: a window the WM moved
        // under a held finger still gets coordinates local to itself.
        build_scene();
        double ox = 0, oy = 0;
        if (lm::surface_from_id(slot->surface) != nullptr &&
            surface_origin(slot->surface, ox, oy)) {
            seat->touch_motion(e.id, x - ox, y - oy);
        }
        push_touch(EventKind::TouchMotion, *slot, x, y);
    }

    void touch_up(const lm::TouchUpEvent& e) {
        idle_notify->notify_activity();
        const TouchSlot* slot = touch_slot(e.id);
        if (slot == nullptr) {
            return;
        }
        seat->touch_up(e.id);
        push_touch(EventKind::TouchUp, *slot, 0, 0);
        std::erase_if(touch_slots, [&](const TouchSlot& t) { return t.id == e.id; });
    }

    /// Every point is gone and none will get an up: a device went away, or
    /// the VT did. Clients are told to forget the whole sequence, and Rust
    /// sees each point lift so a gesture of its own cannot stay half-done.
    void cancel_touches() {
        if (touch_slots.empty()) {
            return;
        }
        seat->touch_cancel();
        for (const TouchSlot& t : touch_slots) {
            push_touch(EventKind::TouchUp, t, 0, 0);
        }
        touch_slots.clear();
    }

    void watch_input(lm::WaylandBackend& host) {
        on_motion = host.pointer_motion.connect([this](lm::PointerMotionAbsEvent& e) {
            if (e.x < 0) { // left the host window
                pointer_left();
                return;
            }
            OutputEntry* first = outputs.empty() ? nullptr : outputs.front().get();
            const lm::Box origin = first == nullptr || first->output == nullptr
                                       ? lm::Box{}
                                       : layout.box_of(*first->output);
            cursor->move_to(origin.x + e.x, origin.y + e.y);
            pointer_moved();
        });
        on_button = host.pointer_button.connect(
            [this](lm::PointerButtonEvent& e) { button(e.button, e.pressed); });
        on_axis = host.pointer_axis.connect([this](lm::PointerAxisEvent& e) { axis(e); });
        on_key = host.key.connect([this](lm::KeyEvent& e) { key(e.keycode, e.pressed); });
        on_mods = host.modifiers.connect([this](lm::ModifiersEvent& e) {
            if (keymap.has_value()) {
                keymap->update_modifiers(e);
            }
            modifiers(e);
        });
        // Adopt the host's layout: the modifier masks above are computed
        // against it, and clients should see the layout the user types on.
        on_keymap = host.keymap_changed.connect([this](lm::KeymapChange& e) { use_keymap(e.text); });
        on_close = host.close_requested.connect(
            [this](lm::HostCloseRequest&) { push(EventKind::CloseRequested); });
    }

    void watch_input(lm::LibinputBackend& devices) {
        on_rel_motion = devices.pointer_motion().connect(
            [this](lm::PointerMotionEvent& e) { relative_motion(e.dx, e.dy); });
        on_button = devices.pointer_button().connect(
            [this](lm::PointerButtonEvent& e) { button(e.button, e.pressed); });
        on_axis = devices.pointer_axis().connect([this](lm::PointerAxisEvent& e) { axis(e); });
        on_key = devices.key().connect([this](lm::KeyEvent& e) { key(e.keycode, e.pressed); });
        // libinput's own xkb state already took this; the seat only relays it.
        on_mods = devices.modifiers().connect([this](lm::ModifiersEvent& e) { modifiers(e); });
        on_touch_down = devices.touch_down().connect(
            [this](lm::TouchDownEvent& e) { touch_down(e); });
        on_touch_motion = devices.touch_motion().connect(
            [this](lm::TouchMotionEvent& e) { touch_motion(e); });
        on_touch_up = devices.touch_up().connect([this](lm::TouchUpEvent& e) { touch_up(e); });
        on_touch_frame =
            devices.touch_frame().connect([this](lm::TouchFrameEvent&) { seat->touch_frame(); });
        // libinput cancels per point; wl_touch cancels the whole sequence, so
        // the first cancel takes every point and the rest find none left.
        on_touch_cancel =
            devices.touch_cancel().connect([this](lm::TouchCancelEvent&) { cancel_touches(); });
        // Advertise what is plugged in: a client that binds a wl_pointer no
        // device can drive waits forever.
        on_caps = devices.capabilities_changed().connect([this](lm::InputCapabilities& caps) {
            seat->set_capabilities(caps.keyboard, caps.pointer, caps.touch);
        });
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

    /// Whichever xkb state the keys are being interpreted against: libinput's
    /// on bare metal, ours (built from the host's keymap) when nested.
    const lm::KeymapState* key_state() const {
        if (input.has_value()) {
            return &input->keymap_state();
        }
        return keymap.has_value() ? &*keymap : nullptr;
    }

    /// The input method holding the keyboard, if one is: it gets every key
    /// a binding does not, and sends text back through text-input.
    lm::InputMethodKeyboardGrab* im_grab() {
        lm::InputMethod* im = input_method->current();
        return im != nullptr && !locked() ? im->keyboard_grab() : nullptr;
    }

    void modifiers(const lm::ModifiersEvent& e) {
        if (lm::InputMethodKeyboardGrab* grab = im_grab(); grab != nullptr) {
            grab->send_modifiers(e.depressed, e.latched, e.locked, e.group);
        } else {
            seat->notify_modifiers(e.depressed, e.latched, e.locked, e.group);
        }
    }

    /// Every key a client was told is down, as up — before the input it came
    /// from goes away (a VT switch), so nothing stays logically held.
    void release_held_keys() {
        for (const std::uint32_t k : held) {
            seat->notify_key(k, false);
        }
        held.clear();
        seat->notify_modifiers(0, 0, 0, 0);
    }

    void key(std::uint32_t keycode, bool pressed) {
        idle_notify->notify_activity();
        if (!pressed) {
            if (keys.unconsume(keycode)) { // the release of a key we kept stays ours
                return;
            }
            if (lm::InputMethodKeyboardGrab* grab = im_grab();
                grab != nullptr && std::ranges::find(held, keycode) == held.end()) {
                grab->send_key(now_ms(), keycode, false);
                return;
            }
            std::erase(held, keycode);
            seat->notify_key(keycode, false);
            return;
        }
        // A press of a key we still think we consumed: its release was lost,
        // which is exactly what happens to the key that switched VTs away
        // (input is suspended before it comes up). Forget it, or this press's
        // release would be swallowed and the client would see the key stuck.
        (void)keys.unconsume(keycode);
        if (const lm::KeymapState* ks = key_state(); ks != nullptr) {
            const std::uint32_t raw = ks->keysym(keycode);
            // Ctrl+Alt+Fn, which the keymap itself turns into XF86Switch_VT_n.
            // Mechanism, not policy: on bare metal it is the way out, and it
            // must work whatever the window manager binds — or the lock screen.
            if (session.has_value() && raw >= XKB_KEY_XF86Switch_VT_1 &&
                raw <= XKB_KEY_XF86Switch_VT_12) {
                keys.consume(keycode);
                release_held_keys();
                const int vt = static_cast<int>(raw - XKB_KEY_XF86Switch_VT_1) + 1;
                if (auto s = session->switch_to(vt); !s) {
                    warn("VT switch", s.error());
                }
                return;
            }
            // No window-manager shortcut may act behind the lock screen.
            if (!locked()) {
                const lm::ModifiersEvent m = ks->modifiers();
                const std::uint32_t mods = (m.depressed | m.latched) & kBindableMods;
                const std::uint32_t sym = xkb_keysym_to_lower(raw);
                for (const Binding& b : bindings) {
                    if (b.mods == mods && b.keysym == sym) {
                        keys.consume(keycode);
                        push(EventKind::Binding);
                        events.back().code = b.id;
                        return;
                    }
                }
            }
        }
        if (lm::InputMethodKeyboardGrab* grab = im_grab(); grab != nullptr) {
            grab->send_key(now_ms(), keycode, true);
            return;
        }
        held.push_back(keycode);
        seat->notify_key(keycode, true);
    }

    // ------------------------------------------------------------- setup

    /// The bare-metal pair. On failure everything is left as it was, so the
    /// caller can still fall back to headless.
    std::string open_drm(const Config& config, bool need_session) {
        if (auto s = lm::Session::create(display->event_loop()); s) {
            session.emplace(std::move(*s));
        } else if (need_session) {
            return "session: " + s.error().message;
        } else {
            // Works from a logged-in VT, but a VT switch is then unsafe.
            warn("libseat session (none; VT switching disabled)", s.error());
        }
        lm::Session* seat_session = session.has_value() ? &*session : nullptr;
        const std::string device(config.drm_device);
        auto drm = device.empty()
                       ? lm::DrmBackend::create(display->event_loop(), seat_session)
                       : lm::DrmBackend::create(display->event_loop(), device, seat_session);
        if (!drm) {
            session.reset();
            return drm.error().message;
        }
        auto devices = lm::LibinputBackend::create(display->event_loop(), seat_session);
        if (!devices) {
            session.reset();
            return "libinput: " + devices.error().message;
        }
        backend = std::make_unique<lm::DrmBackend>(std::move(*drm));
        input.emplace(std::move(*devices));
        if (session.has_value()) {
            on_session = session->activity().connect([this](lm::SessionActive& e) {
                if (!e.active) {
                    release_held_keys();
                    cancel_touches();
                } else {
                    schedule_all(); // back on our VT: repaint what the other one drew over
                }
                push(EventKind::SessionActive);
                events.back().on = e.active;
            });
        }
        return {};
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

        // Every global a desktop expects. A failure in any of them is a
        // broken install, not a runtime choice.
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
        LUM_CREATE(layer_shell, lm::LayerShell::create(*display), "layer-shell");
        LUM_CREATE(single_pixel, lm::SinglePixelBufferManager::create(*display),
                   "single-pixel-buffer");
        LUM_CREATE(presentation, lm::Presentation::create(*display), "presentation-time");
        LUM_CREATE(viewporter, lm::Viewporter::create(*display), "wp_viewporter");
        LUM_CREATE(fractional, lm::FractionalScaleManager::create(*display), "fractional-scale");
        LUM_CREATE(decoration, lm::XdgDecorationManager::create(*display), "xdg-decoration");
        LUM_CREATE(cursor_shape, lm::CursorShapeManager::create(*display), "cursor-shape");
        LUM_CREATE(fifo, lm::FifoManager::create(*display), "wp-fifo");
        LUM_CREATE(commit_timing, lm::CommitTimingManager::create(*display), "wp-commit-timing");
        LUM_CREATE(tearing, lm::TearingControlManager::create(*display), "tearing-control");
        LUM_CREATE(content_type, lm::ContentTypeManager::create(*display), "content-type");
        LUM_CREATE(background_effect, lm::BackgroundEffectManager::create(*display),
                   "background-effect");
        LUM_CREATE(activation, lm::XdgActivation::create(*display), "xdg-activation");
        LUM_CREATE(icons, lm::XdgToplevelIconManager::create(*display), "xdg-toplevel-icon");
        LUM_CREATE(idle_inhibit, lm::IdleInhibitManager::create(*display), "idle-inhibit");
        LUM_CREATE(idle_notify, lm::IdleNotifier::create(*display, display->event_loop()),
                   "idle-notify");
        LUM_CREATE(screencopy, lm::ScreencopyManager::create(*display), "screencopy");
        LUM_CREATE(foreign, lm::ForeignToplevelManager::create(*display), "foreign-toplevel");
        LUM_CREATE(workspaces, lm::WorkspaceManager::create(*display), "ext-workspace");
        LUM_CREATE(output_manager, lm::OutputManager::create(*display, layout),
                   "wlr-output-management");
        LUM_CREATE(locks, lm::SessionLockManager::create(*display), "session-lock");
        LUM_CREATE(seat, lm::Seat::create(*display), "wl_seat");
        LUM_CREATE(data_device, lm::DataDeviceManager::create(*display, *seat), "data-device");
        LUM_CREATE(primary_selection, lm::PrimarySelectionManager::create(*display, *seat),
                   "primary-selection");
        LUM_CREATE(data_control,
                   lm::DataControlManager::create(*display, *data_device, &*primary_selection),
                   "data-control");
        LUM_CREATE(constraints, lm::PointerConstraints::create(*display, *seat),
                   "pointer-constraints");
        LUM_CREATE(relative, lm::RelativePointerManager::create(*display, *seat),
                   "relative-pointer");
        LUM_CREATE(text_input, lm::TextInputManager::create(*display, *seat), "text-input");
        LUM_CREATE(input_method, lm::InputMethodManager::create(*display, *seat, *text_input),
                   "input-method");
#undef LUM_CREATE

        // Optional: only with a GPU, and the compositor works without either.
        if (renderer.gpu() != nullptr) {
            if (auto r = lm::LinuxDmabuf::create(*display, renderer.gpu()); r) {
                dmabuf.emplace(std::move(*r));
            } else {
                warn("linux-dmabuf (off)", r.error());
            }
            screencopy->set_renderer(renderer.gpu()); // dmabuf capture targets too
        }
        if (auto r = lm::DrmSyncobjManager::create(*display); r) {
            syncobj.emplace(std::move(*r));
        }

        decoration->set_default_mode(config.server_side_decorations
                                         ? lm::DecorationMode::server_side
                                         : lm::DecorationMode::client_side);
        renderer.set_presentation(&*presentation);
        cursor.emplace(renderer, layout, *seat, &*cursor_shape, &*screencopy);
        workspace_group = workspaces->add_group();
        workspaces->done();
        foreign->track(*shell); // taskbars see every toplevel without our help

        on_new_toplevel = shell->new_toplevel().connect(
            [this](lm::NewToplevel& e) { watch_toplevel(e.toplevel); });
        on_new_popup =
            shell->new_popup().connect([this](lm::NewPopup& e) { watch_popup(e.popup); });
        on_new_layer = layer_shell->new_layer_surface().connect(
            [this](lm::NewLayerSurface& e) { watch_layer(e.layer_surface); });
        on_foreign = foreign->request().connect(
            [this](lm::ForeignToplevelRequest& r) { foreign_request(r); });
        on_activate = activation->request_activate().connect([this](lm::ActivationRequest& r) {
            // Only a valid token (one the focused client handed over) may move
            // focus; anything else would let any client steal the keyboard.
            if (Window* w = window_of(r.surface); w != nullptr && r.token_valid) {
                push(EventKind::RequestActivate, w->id);
            }
        });
        on_output_config = output_manager->request().connect(
            [this](lm::OutputConfigRequest& r) { configure_outputs(r); });
        on_workspace = workspaces->request().connect([this](lm::WorkspaceRequest& r) {
            if (r.kind == lm::WorkspaceRequest::Kind::activate) {
                push(EventKind::WorkspaceActivate);
                events.back().code = r.workspace;
            }
        });
        on_inhibit = idle_inhibit->changed().connect(
            [this](lm::IdleInhibitChange& e) { idle_notify->set_inhibited(e.inhibited); });
        on_new_lock =
            locks->new_lock().connect([this](lm::NewSessionLock& e) { watch_lock(e.lock); });
        screencopy->set_cursor_source(
            [this](wl_resource* out, lm::CursorCapture& c) { return capture_cursor(out, c); });
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

        // Backend. Auto tries, in order: a window in the parent compositor;
        // the bare-metal pair, but only with a libseat session, i.e. when this
        // process really sits on a seat (a VT, not ssh or CI); headless.
        const int width = config.width > 0 ? config.width : 1280;
        const int height = config.height > 0 ? config.height : 800;
        const BackendKind want = config.backend;
        if (want == BackendKind::Auto || want == BackendKind::Nested) {
            if (auto wb = lm::WaylandBackend::create(display->event_loop()); wb) {
                auto owned = std::make_unique<lm::WaylandBackend>(std::move(*wb));
                nested = owned.get();
                nested->add_output(width, height, "luminaria");
                backend = std::move(owned);
            } else if (want == BackendKind::Nested) {
                return "nested backend: " + wb.error().message;
            }
        }
        if (backend == nullptr && (want == BackendKind::Auto || want == BackendKind::Drm)) {
            if (std::string why = open_drm(config, want == BackendKind::Auto); !why.empty()) {
                if (want == BackendKind::Drm) {
                    return why;
                }
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
        if (input.has_value()) {
            watch_input(*input);
            // The backend computes modifier masks against ITS keymap, so the
            // seat must hand clients the same one.
            if (!seat->set_keymap(input->keymap())) {
                std::fprintf(stderr, "luminaria-rs: libinput keymap rejected by the seat\n");
            }
        }

        auto socket_name = display->add_socket_auto();
        if (!socket_name) {
            return "socket: " + socket_name.error().message;
        }
        socket = std::move(*socket_name);

        if (input.has_value()) {
            if (auto s = input->start(); !s) {
                return "libinput: " + s.error().message;
            }
            const lm::InputCapabilities caps = input->capabilities();
            seat->set_capabilities(caps.keyboard, caps.pointer, caps.touch);
        }
        if (auto s = backend->start(); !s) {
            return "backend: " + s.error().message;
        }
        if (nested != nullptr && !nested->keymap().empty()) {
            use_keymap(nested->keymap()); // already arrived during start()
        } else if (!input.has_value()) {
            if (auto k = lm::KeymapState::from_layout(); k) {
                keymap.emplace(std::move(*k));
            }
        }
        return {};
    }

    ~Impl() {
        // Clients first, while every global still exists. Display's own
        // destructor would do this too, but only after the members above it
        // are gone, and client resources call back into their globals as they
        // die: a clipboard manager still connected at exit (wl-copy serving
        // its selection) has data-control ask the already-freed
        // DataDeviceManager what the selection is.
        if (display.has_value()) {
            wl_display_destroy_clients(display->c_ptr());
        }
        // Then per-object state, explicitly rather than by member order, so no
        // slot can fire into a half-destroyed Impl. Outputs leave the
        // workspace group before their wl_output goes, since it points at it.
        windows.clear();
        popups.clear();
        layer_entries.clear();
        lock_surfaces.clear();
        for (const auto& o : outputs) {
            drop_global(*o);
        }
        outputs.clear();
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
    self.layer_focus = {}; // choosing a window takes the keyboard back from a panel
    if (self.focused != next_id) {
        if (Window* old = self.window(self.focused); old != nullptr) {
            old->toplevel->set_activated(false);
        }
        self.focused = next_id;
        if (next != nullptr) {
            next->toplevel->set_activated(true);
        }
    }
    self.apply_keyboard_focus();
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

void Server::set_minimized(std::uint64_t id, bool on) {
    if (Window* w = impl_->window(id); w != nullptr) {
        w->toplevel->set_minimized(on);
    }
}

void Server::set_tiled(std::uint64_t id, bool on) {
    if (Window* w = impl_->window(id); w != nullptr) {
        w->toplevel->set_tiled(on);
    }
}

void Server::set_resizing(std::uint64_t id, bool on) {
    if (Window* w = impl_->window(id); w != nullptr) {
        w->toplevel->set_resizing(on);
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

rust::String Server::window_icon_name(std::uint64_t id) const {
    const Window* w = impl_->window(id);
    const lm::ToplevelIcon* icon = w != nullptr ? impl_->icons->icon_for(*w->toplevel) : nullptr;
    return icon != nullptr ? to_rust(icon->name) : rust::String();
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

Size Server::window_min_size(std::uint64_t id) const {
    Size size{};
    if (const Window* w = impl_->window(id); w != nullptr) {
        size.width = w->toplevel->min_width();
        size.height = w->toplevel->min_height();
    }
    return size;
}

std::uint64_t Server::window_parent(std::uint64_t id) const {
    const Window* w = impl_->window(id);
    const lm::Toplevel* parent = w != nullptr ? w->toplevel->parent() : nullptr;
    const Window* pw = parent != nullptr ? impl_->window_of(*parent) : nullptr;
    return pw != nullptr ? pw->id : 0;
}

void Server::grab_pointer(bool on) {
    impl_->wm_grab = on;
    if (!on) {
        impl_->update_pointer_focus(); // hand the pointer back to whatever is under it
    }
}

std::uint32_t Server::bind_key(std::uint32_t mods, std::uint32_t keysym) {
    const auto id = static_cast<std::uint32_t>(impl_->bindings.size() + 1);
    impl_->bindings.push_back({id, mods & kBindableMods, xkb_keysym_to_lower(keysym)});
    return id;
}

bool Server::set_keyboard_layout(rust::Str layout) {
    Impl& self = *impl_;
    if (!self.input.has_value()) {
        return false; // nested: the host's layout is what the keys arrive in
    }
    auto k = lm::KeymapState::from_layout(std::string(layout));
    if (!k || !self.input->set_keymap(k->text())) {
        return false;
    }
    return self.seat->set_keymap(self.input->keymap());
}

Rect Server::output_box(std::uint64_t id) const {
    const OutputEntry* o = impl_->output(id);
    return o != nullptr ? to_rect(impl_->layout.box_of(*o->output)) : Rect{};
}

Rect Server::output_usable(std::uint64_t id) const {
    const OutputEntry* o = impl_->output(id);
    return o != nullptr ? to_rect(o->usable) : Rect{};
}

rust::Vec<Mode> Server::output_modes(std::uint64_t id) const {
    rust::Vec<Mode> out;
    if (const OutputEntry* o = impl_->output(id); o != nullptr) {
        for (const lm::OutputMode& m : o->output->modes()) {
            out.push_back(Mode{m.width, m.height, m.refresh_mhz, m.preferred});
        }
    }
    return out;
}

bool Server::set_output_mode(std::uint64_t id, std::int32_t width, std::int32_t height,
                             std::int32_t refresh_mhz, rust::String& error) {
    OutputEntry* o = impl_->output(id);
    if (o == nullptr) {
        error = "no such output";
        return false;
    }
    // Success lands in Output::mode_changed, which rebuilds everything.
    if (auto s = o->output->set_mode(width, height, refresh_mhz); !s) {
        error = to_rust(s.error().message);
        return false;
    }
    return true;
}

void Server::set_output_scale(std::uint64_t id, std::int32_t scale) {
    if (OutputEntry* o = impl_->output(id); o != nullptr) {
        impl_->set_scale(*o, scale);
    }
}

void Server::set_output_transform(std::uint64_t id, std::uint32_t transform) {
    if (OutputEntry* o = impl_->output(id); o != nullptr && transform <= 7) {
        impl_->set_transform(*o, static_cast<lm::Transform>(transform));
    }
}

void Server::set_output_position(std::uint64_t id, std::int32_t x, std::int32_t y) {
    if (OutputEntry* o = impl_->output(id); o != nullptr) {
        impl_->set_position(*o, x, y);
    }
}

std::uint32_t Server::add_workspace(rust::Str name) {
    Impl& self = *impl_;
    const std::string n(name);
    const std::uint32_t id = self.workspaces->add_workspace(self.workspace_group, n, n);
    self.workspaces->done();
    return id;
}

void Server::remove_workspace(std::uint32_t id) {
    impl_->workspaces->remove_workspace(id);
    impl_->workspaces->done();
}

void Server::activate_workspace(std::uint32_t id) {
    impl_->workspaces->activate(id);
    impl_->workspaces->done();
}

bool Server::is_locked() const { return impl_->locked(); }

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

#include "luminaria_rs_shims.inc"

} // namespace lum
