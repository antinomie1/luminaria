// luminaria/pointer_constraints.cppm — zwp_pointer_constraints_v1: pinning the
// pointer to a surface.
//
// Two constraints, one protocol:
//
//   * LOCKED   — the cursor stops moving entirely. The client keeps getting
//     relative motion (see :relative_pointer) and usually hides the cursor.
//     This is mouselook in a game, or dragging a slider past the screen edge.
//   * CONFINED — the cursor still moves and still generates wl_pointer.motion,
//     but cannot leave the surface (or a region of it). This is a VM window or
//     a remote-desktop viewer.
//
// The client only ASKS. A constraint is inert until the compositor calls
// `activate()`, and the compositor must never activate one whose surface does
// not have both pointer and keyboard focus — otherwise any background client
// could capture the mouse. `deactivate()` gives it back; the escape hatch (a
// key chord that breaks out) is the compositor's, and this library deliberately
// does not pick one for you.
//
// Lifetime says what happens after deactivation: a `oneshot` constraint is dead
// and the object is destroyed, a `persistent` one can be activated again when
// focus comes back.
//
// Two invariants the manager enforces so you don't have to: at most one
// constraint per (surface, pointer) pair — a second request is a protocol error
// — and at most one ACTIVE constraint at a time, since there is one cursor.

module;

#include "detail/wayland_fwd.h"

#include <cstdint>
#include <typeinfo>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "pointer-constraints-unstable-v1-protocol.h"

export module luminaria:pointer_constraints;

import std;

import :compositor;
import :display;
import :expected;
import :protocol_helper;
import :region;
import :seat;
import :signal;

export namespace luminaria {

class Display;
class Seat;
class Surface;
class PointerConstraint;

enum class PointerConstraintType {
    locked,
    confined,
};

/// What a constraint is worth after it has been deactivated once.
enum class PointerConstraintLifetime {
    /// Single use. Deactivating leaves it permanently defunct — `activate()`
    /// will not take again — and the client is expected to destroy the object
    /// and ask for a new one. The object itself stays alive until it does.
    oneshot,
    /// Reusable: it goes inert on deactivation and may be activated again when
    /// focus comes back.
    persistent,
};

struct NewPointerConstraint {
    PointerConstraint& constraint;
};
struct PointerConstraintDestroy {
    PointerConstraint& constraint;
};
/// The client changed the region the pointer is confined to (or, for a lock,
/// the area within which it may be placed). Re-clamp the cursor.
struct PointerConstraintRegionChange {
    PointerConstraint& constraint;
};

/// One zwp_locked_pointer_v1 or zwp_confined_pointer_v1. Owned by its resource;
/// address stable for its lifetime, so signals may capture `PointerConstraint&`.
class PointerConstraint {
public:
    virtual ~PointerConstraint() = default;
    PointerConstraint(const PointerConstraint&) = delete;
    PointerConstraint& operator=(const PointerConstraint&) = delete;

    Signal<PointerConstraintDestroy> destroy;
    Signal<PointerConstraintRegionChange> region_change;

    [[nodiscard]] virtual Surface& surface() noexcept = 0;
    [[nodiscard]] virtual const Surface& surface() const noexcept = 0;

    /// The wl_pointer the client named. Only that client's pointer is
    /// constrained; the seat itself is not exclusive.
    [[nodiscard]] virtual wl_resource* pointer_resource() const noexcept = 0;
    [[nodiscard]] virtual PointerConstraintType type() const noexcept = 0;
    [[nodiscard]] virtual PointerConstraintLifetime lifetime() const noexcept = 0;

    /// The area of the surface the pointer is held in, in surface coordinates.
    /// Only meaningful when `has_region()` — the default is the whole surface,
    /// which no finite region can express (same convention as
    /// `Surface::input_region()`).
    [[nodiscard]] virtual const Region& region() const noexcept = 0;
    [[nodiscard]] virtual bool has_region() const noexcept = 0;

    /// Where a LOCKED client would like the cursor left when the lock ends, in
    /// surface coordinates. Only set if the client asked; honouring it is
    /// optional but is what stops the cursor jumping on unlock.
    [[nodiscard]] virtual bool has_cursor_position_hint() const noexcept = 0;
    [[nodiscard]] virtual double cursor_hint_x() const noexcept = 0;
    [[nodiscard]] virtual double cursor_hint_y() const noexcept = 0;

    [[nodiscard]] virtual bool active() const noexcept = 0;

    /// Take the pointer, and tell the client we did. Refused — silently, this
    /// is not an error — unless the surface currently holds the seat's pointer
    /// focus: activating a constraint for an unfocused surface is exactly the
    /// mouse-capture attack the protocol has to not allow. Any other active
    /// constraint is deactivated first, because there is one cursor. No-op if
    /// already active, or if a `oneshot` constraint has already been used.
    virtual void activate() = 0;
    /// Give the pointer back. A `oneshot` constraint is defunct afterwards; a
    /// `persistent` one can be activated again. The object stays alive either
    /// way — only the client destroys it.
    virtual void deactivate() = 0;

protected:
    PointerConstraint() = default;
};

/// The zwp_pointer_constraints_v1 global (version 1). Move-only; pointer-stable
/// state.
class PointerConstraints {
public:
    /// Create the global. `seat` is watched so that a constraint deactivates
    /// itself the moment its surface loses pointer focus — the compositor
    /// cannot forget to, which is the whole security property.
    [[nodiscard]] static Result<PointerConstraints> create(Display& display, Seat& seat);

    ~PointerConstraints();
    PointerConstraints(PointerConstraints&&) noexcept;
    PointerConstraints& operator=(PointerConstraints&&) noexcept;
    PointerConstraints(const PointerConstraints&) = delete;
    PointerConstraints& operator=(const PointerConstraints&) = delete;

    /// A client asked for a constraint. It is inert: call `activate()` on it
    /// when (and only when) the surface deserves the pointer.
    [[nodiscard]] Signal<NewPointerConstraint>& new_constraint() noexcept;

    /// The constraint a client holds on `surface`, or null. Use it from your
    /// focus handler to activate one as focus arrives.
    [[nodiscard]] PointerConstraint* constraint_for(Surface& surface) noexcept;
    /// The constraint that currently owns the pointer, or null. While this is
    /// non-null and locked, do not move the cursor.
    [[nodiscard]] PointerConstraint* active_constraint() noexcept;
    [[nodiscard]] const PointerConstraint* active_constraint() const noexcept;


    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit PointerConstraints(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Implements zwp_pointer_constraints_v1 (version 1): zwp_locked_pointer_v1 and
// zwp_confined_pointer_v1 share every field, so they share one class and differ
// only in which pair of events they send.
//
// The region and the cursor hint are DOUBLE-BUFFERED surface state — the
// protocol says so explicitly — which is why each constraint subscribes to its
// surface's commit rather than applying set_region as it arrives.

namespace luminaria {

// External linkage: PointerConstraints::Impl holds these.
class ConstraintImpl;

struct PointerConstraints::Impl {
    WlGlobal global;
    Seat* seat = nullptr;
    std::vector<ConstraintImpl*> constraints;
    ConstraintImpl* active = nullptr;

    Signal<NewPointerConstraint> new_constraint;
    Signal<SeatPointerFocus>::Connection focus_conn;
};

using PcMgr = PointerConstraints::Impl;

// One zwp_locked_pointer_v1 / zwp_confined_pointer_v1, owned by its resource.
class ConstraintImpl final : public PointerConstraint {
public:
    ConstraintImpl(PcMgr* mgr, Surface& surface, wl_resource* pointer, wl_resource* resource,
                   PointerConstraintType type, PointerConstraintLifetime lifetime)
        : mgr_(mgr), surface_(&surface), pointer_(pointer), resource_(resource), type_(type),
          lifetime_(lifetime) {
        // A constraint without its surface means nothing; take the object down
        // with it rather than keeping a null one around.
        surface_gone_ = surface.destroy.connect([this](SurfaceDestroy&) {
            surface_ = nullptr;
            wl_resource_destroy(resource_);
        });
        commit_conn_ = surface.commit.connect([this](SurfaceCommit&) { apply_pending(); });
    }

    Surface& surface() noexcept override { return *surface_; }
    const Surface& surface() const noexcept override { return *surface_; }

    [[nodiscard]] wl_resource* pointer_resource() const noexcept override { return pointer_; }
    [[nodiscard]] PointerConstraintType type() const noexcept override { return type_; }
    [[nodiscard]] PointerConstraintLifetime lifetime() const noexcept override {
        return lifetime_;
    }
    [[nodiscard]] const Region& region() const noexcept override { return region_; }
    [[nodiscard]] bool has_region() const noexcept override { return has_region_; }
    [[nodiscard]] bool has_cursor_position_hint() const noexcept override { return has_hint_; }
    [[nodiscard]] double cursor_hint_x() const noexcept override { return hint_x_; }
    [[nodiscard]] double cursor_hint_y() const noexcept override { return hint_y_; }
    [[nodiscard]] bool active() const noexcept override { return active_; }

    void activate() override {
        if (active_ || defunct_ || surface_ == nullptr) {
            return;
        }
        // The security rule, enforced here so no compositor can forget it.
        if (mgr_->seat->pointer_focus() != surface_->id()) {
            return;
        }
        if (mgr_->active != nullptr && mgr_->active != this) {
            mgr_->active->deactivate();
        }
        active_ = true;
        mgr_->active = this;
        if (type_ == PointerConstraintType::locked) {
            zwp_locked_pointer_v1_send_locked(resource_);
        } else {
            zwp_confined_pointer_v1_send_confined(resource_);
        }
    }

    void deactivate() override {
        if (!active_) {
            return;
        }
        active_ = false;
        if (mgr_->active == this) {
            mgr_->active = nullptr;
        }
        if (lifetime_ == PointerConstraintLifetime::oneshot) {
            defunct_ = true;
        }
        if (type_ == PointerConstraintType::locked) {
            zwp_locked_pointer_v1_send_unlocked(resource_);
        } else {
            zwp_confined_pointer_v1_send_unconfined(resource_);
        }
    }

    /// set_region / set_cursor_position_hint land here on the next commit.
    void apply_pending() {
        bool changed = false;
        if (pending_region_set_) {
            region_ = pending_region_;
            has_region_ = pending_has_region_;
            pending_region_set_ = false;
            changed = true;
        }
        if (pending_hint_set_) {
            has_hint_ = true;
            hint_x_ = pending_hint_x_;
            hint_y_ = pending_hint_y_;
            pending_hint_set_ = false;
        }
        if (changed) {
            PointerConstraintRegionChange event{*this};
            region_change.emit(event);
        }
    }

    void stage_region(Region* region) {
        pending_region_set_ = true;
        pending_has_region_ = region != nullptr;
        pending_region_ = region != nullptr ? *region : Region{};
    }
    void stage_cursor_hint(double x, double y) {
        pending_hint_set_ = true;
        pending_hint_x_ = x;
        pending_hint_y_ = y;
    }

    PcMgr* mgr_ = nullptr;
    Surface* surface_ = nullptr;
    wl_resource* pointer_ = nullptr;
    wl_resource* resource_ = nullptr;
    PointerConstraintType type_;
    PointerConstraintLifetime lifetime_;

    Region region_;
    bool has_region_ = false;
    Region pending_region_;
    bool pending_has_region_ = false;
    bool pending_region_set_ = false;

    bool has_hint_ = false;
    double hint_x_ = 0.0;
    double hint_y_ = 0.0;
    bool pending_hint_set_ = false;
    double pending_hint_x_ = 0.0;
    double pending_hint_y_ = 0.0;

    bool active_ = false;
    bool defunct_ = false;

    Signal<SurfaceDestroy>::Connection surface_gone_;
    Signal<SurfaceCommit>::Connection commit_conn_;
};

namespace {

ConstraintImpl* constraint_of(wl_resource* resource) {
    return static_cast<ConstraintImpl*>(wl_resource_get_user_data(resource));
}

void constraint_set_region(wl_client*, wl_resource* resource, wl_resource* region) {
    constraint_of(resource)->stage_region(region_from_resource(region));
}

void locked_set_cursor_position_hint(wl_client*, wl_resource* resource, wl_fixed_t x,
                                     wl_fixed_t y) {
    constraint_of(resource)->stage_cursor_hint(wl_fixed_to_double(x), wl_fixed_to_double(y));
}

constexpr struct zwp_locked_pointer_v1_interface locked_impl = {
    .destroy = resource_destroy_request,
    .set_cursor_position_hint = locked_set_cursor_position_hint,
    .set_region = constraint_set_region,
};

constexpr struct zwp_confined_pointer_v1_interface confined_impl = {
    .destroy = resource_destroy_request,
    .set_region = constraint_set_region,
};

void constraint_resource_destroy(wl_resource* resource) {
    ConstraintImpl* constraint = constraint_of(resource);
    PointerConstraintDestroy event{*constraint};
    constraint->destroy.emit(event);
    PcMgr* mgr = constraint->mgr_;
    if (mgr->active == constraint) {
        mgr->active = nullptr;
    }
    std::erase(mgr->constraints, constraint);
    delete constraint;
}

PointerConstraintLifetime lifetime_from_wire(uint32_t lifetime) {
    return lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT
               ? PointerConstraintLifetime::oneshot
               : PointerConstraintLifetime::persistent;
}

// lock_pointer and confine_pointer differ only in the interface of the object
// they create, so one function builds both.
void create_constraint(wl_client* client, wl_resource* manager_resource, uint32_t id,
                       wl_resource* surface_resource, wl_resource* pointer_resource,
                       wl_resource* region_resource, uint32_t lifetime,
                       PointerConstraintType type) {
    auto* mgr = static_cast<PcMgr*>(wl_resource_get_user_data(manager_resource));
    Surface* surface = surface_from_resource(surface_resource);
    if (surface == nullptr) {
        // libwayland already type-checked the argument against wl_surface, so
        // this only happens for a wl_surface that is not one of ours. There is
        // no protocol error that fits; drop the request.
        return;
    }
    // One constraint per (surface, pointer): the protocol makes a second one a
    // fatal error rather than letting two clients fight over the cursor.
    const bool taken = std::ranges::any_of(mgr->constraints, [&](const ConstraintImpl* c) {
        return c->surface_ == surface && c->pointer_ == pointer_resource;
    });
    if (taken) {
        wl_resource_post_error(manager_resource,
                               ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,
                               "this surface and pointer are already constrained");
        return;
    }

    const wl_interface* interface = type == PointerConstraintType::locked
                                        ? &zwp_locked_pointer_v1_interface
                                        : &zwp_confined_pointer_v1_interface;
    wl_resource* resource = wl_resource_create(
        client, interface, wl_resource_get_version(manager_resource), static_cast<int>(id));
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* constraint = new ConstraintImpl(mgr, *surface, pointer_resource, resource, type,
                                          lifetime_from_wire(lifetime));
    // The initial region is not double-buffered — it is an argument of the
    // creating request, so it is in force immediately.
    if (Region* region = region_from_resource(region_resource); region != nullptr) {
        constraint->region_ = *region;
        constraint->has_region_ = true;
    }
    wl_resource_set_implementation(
        resource, type == PointerConstraintType::locked ? static_cast<const void*>(&locked_impl)
                                                        : static_cast<const void*>(&confined_impl),
        constraint, constraint_resource_destroy);
    mgr->constraints.push_back(constraint);

    NewPointerConstraint event{*constraint};
    mgr->new_constraint.emit(event);
}

void manager_lock_pointer(wl_client* client, wl_resource* manager_resource, uint32_t id,
                          wl_resource* surface, wl_resource* pointer, wl_resource* region,
                          uint32_t lifetime) {
    create_constraint(client, manager_resource, id, surface, pointer, region, lifetime,
                      PointerConstraintType::locked);
}

void manager_confine_pointer(wl_client* client, wl_resource* manager_resource, uint32_t id,
                             wl_resource* surface, wl_resource* pointer, wl_resource* region,
                             uint32_t lifetime) {
    create_constraint(client, manager_resource, id, surface, pointer, region, lifetime,
                      PointerConstraintType::confined);
}

constexpr struct zwp_pointer_constraints_v1_interface manager_impl = {
    .destroy = resource_destroy_request,
    .lock_pointer = manager_lock_pointer,
    .confine_pointer = manager_confine_pointer,
};

} // namespace

PointerConstraints::PointerConstraints(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
PointerConstraints::~PointerConstraints() = default;
PointerConstraints::PointerConstraints(PointerConstraints&&) noexcept = default;
PointerConstraints& PointerConstraints::operator=(PointerConstraints&&) noexcept = default;

Result<PointerConstraints> PointerConstraints::create(Display& display, Seat& seat) {
    auto impl = std::make_unique<Impl>();
    impl->seat = &seat;
    auto global = create_wl_global<&zwp_pointer_constraints_v1_interface,
                                   default_bind<&zwp_pointer_constraints_v1_interface,
                                                &manager_impl>>(display, 1, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    // Losing pointer focus ends the constraint, always. Leaving that to the
    // compositor is how a client keeps the mouse after the user has clicked
    // somewhere else.
    Impl* raw = impl.get();
    impl->focus_conn = seat.pointer_focus_changed().connect([raw](SeatPointerFocus& e) {
        if (raw->active != nullptr && raw->active->surface_->id() != e.surface) {
            raw->active->deactivate();
        }
    });
    return PointerConstraints{std::move(impl)};
}

Signal<NewPointerConstraint>& PointerConstraints::new_constraint() noexcept {
    return impl_->new_constraint;
}

PointerConstraint* PointerConstraints::constraint_for(Surface& surface) noexcept {
    auto it = std::ranges::find_if(impl_->constraints,
                                   [&](const ConstraintImpl* c) { return c->surface_ == &surface; });
    return it != impl_->constraints.end() ? *it : nullptr;
}

PointerConstraint* PointerConstraints::active_constraint() noexcept {
    return impl_->active;
}

const PointerConstraint* PointerConstraints::active_constraint() const noexcept {
    return impl_->active;
}


} // namespace luminaria
