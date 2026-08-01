module;

// Implements zwp_pointer_constraints_v1 (version 1): zwp_locked_pointer_v1 and
// zwp_confined_pointer_v1 share every field, so they share one class and differ
// only in which pair of events they send.
//
// The region and the cursor hint are DOUBLE-BUFFERED surface state — the
// protocol says so explicitly — which is why each constraint subscribes to its
// surface's commit rather than applying set_region as it arrives.

#include <memory>

#include <algorithm>
#include <cstdint>
#include <typeinfo>
#include <vector>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "pointer-constraints-unstable-v1-protocol.h"

module luminaria;

namespace luminaria {

// External linkage: PointerConstraints::Impl holds these.
class ConstraintImpl;

struct PointerConstraints::Impl {
    wl_global* global = nullptr;
    Seat* seat = nullptr;
    std::vector<ConstraintImpl*> constraints;
    ConstraintImpl* active = nullptr;

    Signal<NewPointerConstraint> new_constraint;
    Signal<SeatPointerFocus>::Connection focus_conn;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

using Mgr = PointerConstraints::Impl;

// One zwp_locked_pointer_v1 / zwp_confined_pointer_v1, owned by its resource.
class ConstraintImpl final : public PointerConstraint {
public:
    ConstraintImpl(Mgr* mgr, Surface& surface, wl_resource* pointer, wl_resource* resource,
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
        if (mgr_->seat->pointer_focus() != surface_) {
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

    Mgr* mgr_ = nullptr;
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

void constraint_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void constraint_set_region(wl_client*, wl_resource* resource, wl_resource* region) {
    constraint_of(resource)->stage_region(region_from_resource(region));
}

void locked_set_cursor_position_hint(wl_client*, wl_resource* resource, wl_fixed_t x,
                                     wl_fixed_t y) {
    constraint_of(resource)->stage_cursor_hint(wl_fixed_to_double(x), wl_fixed_to_double(y));
}

constexpr struct zwp_locked_pointer_v1_interface locked_impl = {
    .destroy = constraint_destroy_request,
    .set_cursor_position_hint = locked_set_cursor_position_hint,
    .set_region = constraint_set_region,
};

constexpr struct zwp_confined_pointer_v1_interface confined_impl = {
    .destroy = constraint_destroy_request,
    .set_region = constraint_set_region,
};

void constraint_resource_destroy(wl_resource* resource) {
    ConstraintImpl* constraint = constraint_of(resource);
    PointerConstraintDestroy event{*constraint};
    constraint->destroy.emit(event);
    Mgr* mgr = constraint->mgr_;
    if (mgr->active == constraint) {
        mgr->active = nullptr;
    }
    std::erase(mgr->constraints, constraint);
    delete constraint;
}

void manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
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
    auto* mgr = static_cast<Mgr*>(wl_resource_get_user_data(manager_resource));
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
    .destroy = manager_destroy_request,
    .lock_pointer = manager_lock_pointer,
    .confine_pointer = manager_confine_pointer,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zwp_pointer_constraints_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

} // namespace

PointerConstraints::PointerConstraints(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
PointerConstraints::~PointerConstraints() = default;
PointerConstraints::PointerConstraints(PointerConstraints&&) noexcept = default;
PointerConstraints& PointerConstraints::operator=(PointerConstraints&&) noexcept = default;

Result<PointerConstraints> PointerConstraints::create(Display& display, Seat& seat) {
    auto impl = std::make_unique<Impl>();
    impl->seat = &seat;
    impl->global = wl_global_create(display.c_ptr(), &zwp_pointer_constraints_v1_interface, 1,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(zwp_pointer_constraints_v1) failed");
    }
    // Losing pointer focus ends the constraint, always. Leaving that to the
    // compositor is how a client keeps the mouse after the user has clicked
    // somewhere else.
    Impl* raw = impl.get();
    impl->focus_conn = seat.pointer_focus_changed().connect([raw](SeatPointerFocus& e) {
        if (raw->active != nullptr && raw->active->surface_ != e.surface) {
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

} // namespace luminaria
