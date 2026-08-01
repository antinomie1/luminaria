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

#include "luminaria/detail/wayland_fwd.h"

#include <memory>

export module luminaria:pointer_constraints;

import :core.expected;
import :core.signal;
import :util.region;

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

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit PointerConstraints(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
