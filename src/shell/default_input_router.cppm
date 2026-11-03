// luminaria/shell/default_input_router.cppm — input, given what a key or a click
// means.
//
// A compositor used to write one `match` over seven event types: motion to the
// `Cursor`, then the scene; a press to the `KeyRouter`, past the VT switch; the
// click-outside-a-menu rule; re-picking the surface under the pointer whenever
// the stack changed or an implicit grab ended. It came out the same every time,
// and the differences between copies were bugs. What is left to the caller is
// what is actually a decision: `bind()` and `on_key_binding()` for the keyboard,
// `on_button()` for the mouse.
//
// It routes against `ScreenManager::scene_at()`, the list the screen is drawn
// from, so what gets clicked is what is shown. Nothing is remembered between
// events but the keys a binding kept.
//
// Pointer constraints and relative motion are opt-in, through
// `follow_constraints()`: a locked pointer stays where it is, a confined one
// stays in its surface (and region), and relative motion goes to the focused
// client whether the cursor moved or not. Touch is not here; there is no
// default for it to be.
module;

#include <xkbcommon/xkbcommon.h>

export module luminaria.shell:default_input_router;

import std;

import luminaria;
import luminaria.backend;

import :auto_backend;
import :cursor;
import :cursor_manager;
import :popup;
import :scene;
import :screen_manager;

export namespace luminaria {

class DefaultInputRouter {
public:
    /// Offered every press no `bind()` took; true keeps the key from the client,
    /// and its release with it.
    using KeyBinding = std::function<bool(const KeyEvent& event)>;
    /// A pointer button, before the client hears of it. `hit` is what is under
    /// the pointer, and like every `SceneHit` its `surface` is good for this call.
    using ButtonHook = std::function<void(std::uint32_t button, bool pressed, const SceneHit& hit)>;
    using FocusHook = std::function<void()>;

    /// Everything is borrowed for the router's whole life, so declare it last.
    DefaultInputRouter(Seat& seat, Cursor& cursor, ScreenManager& screens, XdgShell& shell,
                       PopupTree& popups);
    ~DefaultInputRouter();
    DefaultInputRouter(DefaultInputRouter&&) noexcept;
    DefaultInputRouter& operator=(DefaultInputRouter&&) noexcept;
    DefaultInputRouter(const DefaultInputRouter&) = delete;
    DefaultInputRouter& operator=(const DefaultInputRouter&) = delete;

    /// Take input from `source`, and have `seat` follow its keymap and devices.
    void attach(InputSource& source);
    /// Ctrl+Alt+Fn switches VTs, keys held are released when the VT goes away,
    /// and every output repaints when it comes back. Borrowed.
    void attach(Session& session);
    /// Both, whichever of them `backend` has.
    void attach(AutoBackend& backend);

    /// `mods` (`Mod::logo` and friends, exactly those held) with `keysym` runs
    /// `action`, and the client never sees the key. Caps Lock changes nothing:
    /// `keysym` is compared in lower case.
    void bind(std::uint32_t mods, std::uint32_t keysym, std::function<void()> action);
    void on_key_binding(KeyBinding hook);
    void on_button(ButtonHook hook);
    /// The pointer or the keyboard moved to another surface.
    void on_focus_changed(FocusHook hook);

    /// A press on a window gives it the keyboard and the activated look, through
    /// `XdgShell::focus()`; a press in a popup, which is its parent's business,
    /// changes nothing. On by default. Off for a compositor that keeps focus for
    /// itself.
    void set_click_to_focus(bool enabled) noexcept;

    /// One event, as `attach()` would deliver it.
    void route(const InputEvent& event);

    /// Pick the surface under the pointer again. An `invalidate()` on the
    /// `ScreenManager` does this by itself.
    void update_pointer();

    /// Honour `constraints`: a constraint activates when the pointer enters
    /// its surface (inside its region, if it has one), a locked pointer then
    /// stops moving, and a confined one stays in the surface's region. With
    /// `relative`, every relative motion reaches the focused client as such,
    /// moved or not, which is what games and 3D views read. Both are borrowed.
    void follow_constraints(PointerConstraints& constraints,
                            RelativePointerManager* relative = nullptr);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct InputKeyBind {
    std::uint32_t mods;
    std::uint32_t keysym;
    std::function<void()> action;
};

struct InputKnownToplevel {
    Toplevel* toplevel;
    Signal<ToplevelDestroy>::Connection on_destroy;
};

struct DefaultInputRouter::Impl {
    Seat* seat = nullptr;
    Cursor* cursor = nullptr;
    ScreenManager* screens = nullptr;
    XdgShell* shell = nullptr;
    PopupTree* popups = nullptr;
    Session* session = nullptr;
    KeyRouter keys;
    std::vector<InputKeyBind> bindings;
    KeyBinding on_key;
    ButtonHook on_button;
    FocusHook on_focus;
    bool click_to_focus = true;
    PointerConstraints* constraints = nullptr;
    RelativePointerManager* relative = nullptr;
    // Every toplevel, to get from the root of a hit back to the window.
    std::list<InputKnownToplevel> toplevels;

    Connections connections;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] bool binding(const KeyEvent& event);
    void key(const KeyEvent& event);
    void button(std::uint32_t code, bool pressed);
    void update_pointer();
    void focus_window(const SceneHit& hit);
    void route(const InputEvent& event);
    void motion(const PointerMotionEvent& event);
    void activate_constraint();
    [[nodiscard]] std::optional<Box> confinement(PointerConstraint& constraint);
};

bool DefaultInputRouter::Impl::binding(const KeyEvent& event) {
    if (session != nullptr && session->switch_vt_for(event.keysym)) {
        seat->release_keys(); // their releases arrive on the other VT
        return true;
    }
    const std::uint32_t held = event.modifiers.held();
    const std::uint32_t keysym = xkb_keysym_to_lower(event.keysym);
    for (const InputKeyBind& bound : bindings) {
        if (bound.mods == held && bound.keysym == keysym) {
            bound.action();
            return true;
        }
    }
    return on_key && on_key(event);
}

void DefaultInputRouter::Impl::key(const KeyEvent& event) {
    keys.route(*seat, event, [this](const KeyEvent& e) { return binding(e); });
}

void DefaultInputRouter::Impl::update_pointer() {
    // A locked pointer keeps its surface and hears no motion; the lock ends
    // by itself when that surface loses focus some other way.
    if (constraints != nullptr) {
        if (const PointerConstraint* active = constraints->active_constraint();
            active != nullptr && active->type() == PointerConstraintType::locked) {
            return;
        }
    }
    if (const Pointer& pointer = cursor->pointer(); pointer.present()) {
        scene_route_pointer(*seat, screens->scene_at(pointer.x(), pointer.y()), pointer.x(),
                            pointer.y());
        activate_constraint();
    }
}

// Where a confined pointer may go, in layout coordinates: its surface, narrowed
// to the region's extents when it set one. Null while the surface is not shown.
std::optional<Box> DefaultInputRouter::Impl::confinement(PointerConstraint& constraint) {
    std::optional<Box> box = screens->surface_box(constraint.surface().id());
    if (box && constraint.has_region()) {
        Box region = constraint.region().extents();
        region.x += box->x;
        region.y += box->y;
        *box = box->intersection(region);
    }
    return box;
}

// The pointer just arrived somewhere: a constraint waiting on that surface
// takes it, if the pointer is inside the constraint's region.
void DefaultInputRouter::Impl::activate_constraint() {
    if (constraints == nullptr) {
        return;
    }
    Surface* focus = surface_from_id(seat->pointer_focus());
    if (focus == nullptr) {
        return;
    }
    PointerConstraint* constraint = constraints->constraint_for(*focus);
    if (constraint == nullptr || constraint->active()) {
        return;
    }
    if (constraint->has_region()) {
        const std::optional<Box> box = screens->surface_box(focus->id());
        const Pointer& pointer = cursor->pointer();
        if (!box || !constraint->region().contains(static_cast<int>(pointer.x()) - box->x,
                                                   static_cast<int>(pointer.y()) - box->y)) {
            return;
        }
    }
    constraint->activate();
}

void DefaultInputRouter::Impl::motion(const PointerMotionEvent& event) {
    CursorManager::send_relative_motion(relative, event.dx, event.dy);
    PointerConstraint* active = constraints != nullptr ? constraints->active_constraint() : nullptr;
    if (active != nullptr && active->type() == PointerConstraintType::locked) {
        return; // the cursor stays; the client had the relative motion
    }
    std::optional<Box> bounds = active != nullptr ? confinement(*active) : std::nullopt;
    const bool moved = bounds && !bounds->empty() ? cursor->move_by(event.dx, event.dy, *bounds)
                                                  : cursor->move_by(event);
    if (moved) {
        update_pointer();
    }
}

void DefaultInputRouter::follow_constraints(PointerConstraints& constraints,
                                            RelativePointerManager* relative) {
    impl_->constraints = &constraints;
    impl_->relative = relative;
}

void DefaultInputRouter::Impl::focus_window(const SceneHit& hit) {
    if (hit.surface == nullptr) {
        return;
    }
    Surface* root = hit.surface;
    while (root->subsurface_parent() != nullptr) {
        root = root->subsurface_parent();
    }
    const auto known = std::ranges::find_if(
        toplevels, [root](const InputKnownToplevel& t) { return &t.toplevel->surface() == root; });
    if (known != toplevels.end()) {
        shell->focus(*seat, known->toplevel);
    }
}

void DefaultInputRouter::Impl::button(std::uint32_t code, bool pressed) {
    if (pressed) {
        popups->dismiss_grabs_outside(seat->pointer_focus()); // a click outside a menu closes it
    }
    SceneHit hit;
    if (const Pointer& pointer = cursor->pointer(); pointer.present()) {
        hit = scene_hit_test(screens->scene_at(pointer.x(), pointer.y()), pointer.x(), pointer.y());
    }
    if (pressed && click_to_focus) {
        focus_window(hit);
    }
    if (on_button) {
        on_button(code, pressed, hit);
    }
    if (seat->pointer_focus().valid()) {
        seat->pointer_button(code, pressed);
    }
    if (!pressed) {
        update_pointer(); // an implicit grab may have ended; re-pick
    }
}

DefaultInputRouter::DefaultInputRouter(Seat& seat, Cursor& cursor, ScreenManager& screens,
                                       XdgShell& shell, PopupTree& popups)
    : impl_(std::make_unique<Impl>()) {
    Impl& self = *impl_;
    self.seat = &seat;
    self.cursor = &cursor;
    self.screens = &screens;
    self.shell = &shell;
    self.popups = &popups;
    const auto focus_changed = [&self] {
        if (self.on_focus) {
            self.on_focus();
        }
    };
    self.connections += seat.pointer_focus_changed().connect(focus_changed);
    self.connections += seat.keyboard_focus_changed().connect(focus_changed);
    self.connections += screens.invalidated().connect([&self] { self.update_pointer(); });
    self.connections += shell.new_toplevel().connect([&self](NewToplevel& e) {
        InputKnownToplevel& known = self.toplevels.emplace_back(&e.toplevel);
        known.on_destroy = e.toplevel.destroy.connect([&self, &known](ToplevelDestroy&) {
            std::erase_if(self.toplevels,
                          [&known](const InputKnownToplevel& x) { return &x == &known; });
        });
    });
}

DefaultInputRouter::~DefaultInputRouter() = default;
DefaultInputRouter::DefaultInputRouter(DefaultInputRouter&&) noexcept = default;
DefaultInputRouter& DefaultInputRouter::operator=(DefaultInputRouter&&) noexcept = default;

void DefaultInputRouter::attach(InputSource& source) {
    impl_->seat->follow(source);
    Impl& self = *impl_;
    self.connections += source.input().connect([&self](InputEvent& event) { self.route(event); });
}

void DefaultInputRouter::attach(Session& session) {
    Impl& self = *impl_;
    self.session = &session;
    self.connections += session.activity().connect([&self](SessionActive& e) {
        if (e.active) {
            self.screens->invalidate(); // repaint what the other VT drew over
        } else {
            self.seat->release_keys();
        }
    });
}

void DefaultInputRouter::attach(AutoBackend& backend) {
    if (InputSource* source = backend.input(); source != nullptr) {
        attach(*source);
    }
    if (Session* session = backend.session(); session != nullptr) {
        attach(*session);
    }
}

void DefaultInputRouter::bind(std::uint32_t mods, std::uint32_t keysym,
                              std::function<void()> action) {
    impl_->bindings.push_back({mods, xkb_keysym_to_lower(keysym), std::move(action)});
}

void DefaultInputRouter::on_key_binding(KeyBinding hook) { impl_->on_key = std::move(hook); }

void DefaultInputRouter::on_button(ButtonHook hook) { impl_->on_button = std::move(hook); }

void DefaultInputRouter::on_focus_changed(FocusHook hook) { impl_->on_focus = std::move(hook); }

void DefaultInputRouter::set_click_to_focus(bool enabled) noexcept {
    impl_->click_to_focus = enabled;
}

void DefaultInputRouter::update_pointer() { impl_->update_pointer(); }

void DefaultInputRouter::route(const InputEvent& event) { impl_->route(event); }

void DefaultInputRouter::Impl::route(const InputEvent& event) {
    match(
        event, [this](const KeyEvent& e) { key(e); },
        [this](const ModifiersEvent& e) { seat->notify_modifiers(e); },
        [this](const PointerMotionEvent& e) { motion(e); },
        [this](const PointerMotionAbsEvent& e) {
            cursor->move_to(e);
            update_pointer();
        },
        [this](const PointerLeaveEvent&) {
            cursor->leave();
            seat->pointer_clear_focus();
        },
        [this](const PointerButtonEvent& e) { button(e.button, e.pressed); },
        [this](const PointerAxisEvent& e) { seat->pointer_axis(e); },
        [](const auto&) {});
}

} // namespace luminaria
