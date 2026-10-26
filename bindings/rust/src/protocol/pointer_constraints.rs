//! pointer-constraints: games and 3D views locking or confining the pointer.

use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi::{self, EventKind};
use crate::util::Rect;

use super::{Seat, SurfaceId};

/// Constraints are named by the surface they constrain: a surface has at most
/// one.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PointerConstraintEvent {
    New(SurfaceId),
    Destroy(SurfaceId),
    /// The client committed a new region.
    RegionChange(SurfaceId),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PointerConstraintKind {
    /// The pointer does not move at all; relative motion still flows.
    Locked,
    /// The pointer moves, but stays inside the region.
    Confined,
}

/// The `zwp_pointer_constraints_v1` global. It routes through the [`Seat`] it
/// was created for, which it keeps borrowed. A constraint does nothing until
/// the compositor activates it, normally when its surface has pointer focus.
pub struct PointerConstraints<'a> {
    pub(crate) raw: cxx::UniquePtr<ffi::PointerConstraintsShim>,
    _borrows: Holds<'a>,
}

impl<'a> PointerConstraints<'a> {
    pub fn new(display: &'a Display, seat: &'a Seat<'_>) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::pointer_constraints_new(&display.raw, &seat.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self { raw, _borrows: Holds::new() })
        }
    }
}

impl PointerConstraints<'_> {
    pub fn next_event(&self) -> Option<PointerConstraintEvent> {
        loop {
            let raw = crate::next_raw_event(|out| self.raw.next_event(out))?;
            let surface = SurfaceId(raw.id);
            return Some(match raw.kind {
                EventKind::NewPointerConstraint => PointerConstraintEvent::New(surface),
                EventKind::PointerConstraintDestroy => PointerConstraintEvent::Destroy(surface),
                EventKind::PointerConstraintRegionChange => PointerConstraintEvent::RegionChange(surface),
                _ => continue,
            });
        }
    }

    /// The constraint on `surface`, if there is one now.
    pub fn constraint(&self, surface: SurfaceId) -> Option<PointerConstraint<'_>> {
        let mut info = ffi::PointerConstraintInfoRaw::default();
        self.raw
            .constraint_info(surface.0, &mut info)
            .then_some(PointerConstraint { mgr: self.raw.as_ref()?, surface })
    }

    /// The surface whose constraint is active.
    pub fn active_constraint(&self) -> Option<SurfaceId> {
        SurfaceId::from_raw(self.raw.active_constraint())
    }
}

/// A pointer constraint, looked up by its surface on every call.
#[derive(Clone, Copy)]
pub struct PointerConstraint<'m> {
    mgr: &'m ffi::PointerConstraintsShim,
    surface: SurfaceId,
}

impl PointerConstraint<'_> {
    fn info(&self) -> ffi::PointerConstraintInfoRaw {
        let mut info = ffi::PointerConstraintInfoRaw::default();
        self.mgr.constraint_info(self.surface.0, &mut info);
        info
    }

    pub fn surface(&self) -> SurfaceId {
        self.surface
    }

    pub fn kind(&self) -> PointerConstraintKind {
        if self.info().locked {
            PointerConstraintKind::Locked
        } else {
            PointerConstraintKind::Confined
        }
    }

    /// Whether it survives deactivation (persistent) or dies with it (oneshot).
    pub fn is_persistent(&self) -> bool {
        self.info().persistent
    }

    /// The region in surface coordinates, `None` for the whole surface.
    pub fn region(&self) -> Option<Vec<Rect>> {
        self.info().has_region.then(|| self.mgr.constraint_region(self.surface.0))
    }

    /// Where a locked client would like the cursor drawn when it unlocks.
    pub fn cursor_hint(&self) -> Option<(f64, f64)> {
        let i = self.info();
        i.has_hint.then_some((i.hint_x, i.hint_y))
    }

    pub fn is_active(&self) -> bool {
        self.info().active
    }

    pub fn activate(&self) {
        self.mgr.activate(self.surface.0);
    }

    pub fn deactivate(&self) {
        self.mgr.deactivate(self.surface.0);
    }
}
