//! xdg-shell: application windows (toplevels) and their popups.

use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi::{self, EventKind};
use crate::util::Rect;

use super::SurfaceId;

/// Something a toplevel did, or asked for. The `request_*` kinds are the client
/// asking; the compositor decides, answers through [`Toplevel`]'s setters and
/// [`Toplevel::configure`], and may ignore them.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ToplevelEvent {
    /// The surface took the toplevel role. Nothing is on screen yet.
    Created,
    Map,
    Unmap,
    Destroy,
    RequestMaximize(bool),
    RequestFullscreen(bool),
    RequestMinimize,
    /// Interactive move, started from the click with this input serial.
    RequestMove { serial: u32 },
    /// Interactive resize; `edges` is a [`crate::util::edge`] bitmask.
    RequestResize { serial: u32, edges: u32 },
    /// Right-click on the client's own decorations, at (`x`, `y`) in its
    /// surface: it wants the compositor's window menu.
    RequestWindowMenu { serial: u32, x: i32, y: i32 },
    /// The title or app id changed.
    IdentityChange,
    /// One of the compositor's own setters changed the window's state.
    StateChange,
    /// The client made this window transient for another, or cleared it.
    ParentChange(Option<SurfaceId>),
}

/// Something a popup did.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PopupEvent {
    /// `parent` is the xdg parent's surface, `None` for a popup a layer
    /// surface will claim (see [`LayerSurfaceEvent::NewPopup`](super::LayerSurfaceEvent::NewPopup)).
    Created { parent: Option<SurfaceId> },
    Map,
    Unmap,
    Destroy,
    /// The client repositioned it; its geometry changed.
    Reposition,
    /// It asked for an explicit grab, from the click with this serial.
    Grab { serial: u32 },
}

/// An xdg-shell event ([`crate::Event::Xdg`]), named by the surface it is about.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum XdgEvent {
    Toplevel(SurfaceId, ToplevelEvent),
    Popup(SurfaceId, PopupEvent),
}

impl XdgEvent {
    pub(crate) fn from_raw(raw: &ffi::EventRaw) -> Option<Self> {
        let s = SurfaceId(raw.id);
        let toplevel = |e| Some(Self::Toplevel(s, e));
        let popup = |e| Some(Self::Popup(s, e));
        match raw.kind {
            EventKind::NewToplevel => toplevel(ToplevelEvent::Created),
            EventKind::ToplevelMap => toplevel(ToplevelEvent::Map),
            EventKind::ToplevelUnmap => toplevel(ToplevelEvent::Unmap),
            EventKind::ToplevelDestroy => toplevel(ToplevelEvent::Destroy),
            EventKind::ToplevelRequestMaximize => toplevel(ToplevelEvent::RequestMaximize(raw.flag)),
            EventKind::ToplevelRequestFullscreen => toplevel(ToplevelEvent::RequestFullscreen(raw.flag)),
            EventKind::ToplevelRequestMinimize => toplevel(ToplevelEvent::RequestMinimize),
            EventKind::ToplevelRequestMove => toplevel(ToplevelEvent::RequestMove { serial: raw.serial }),
            EventKind::ToplevelRequestResize => toplevel(ToplevelEvent::RequestResize {
                serial: raw.serial,
                edges: raw.value,
            }),
            EventKind::ToplevelRequestWindowMenu => toplevel(ToplevelEvent::RequestWindowMenu {
                serial: raw.serial,
                x: raw.x,
                y: raw.y,
            }),
            EventKind::ToplevelIdentityChange => toplevel(ToplevelEvent::IdentityChange),
            EventKind::ToplevelStateChange => toplevel(ToplevelEvent::StateChange),
            EventKind::ToplevelParentChange => toplevel(ToplevelEvent::ParentChange(SurfaceId::from_raw(raw.other))),
            EventKind::NewPopup => popup(PopupEvent::Created {
                parent: SurfaceId::from_raw(raw.other),
            }),
            EventKind::PopupMap => popup(PopupEvent::Map),
            EventKind::PopupUnmap => popup(PopupEvent::Unmap),
            EventKind::PopupDestroy => popup(PopupEvent::Destroy),
            EventKind::PopupReposition => popup(PopupEvent::Reposition),
            EventKind::PopupGrab => popup(PopupEvent::Grab { serial: raw.serial }),
            _ => None,
        }
    }
}

/// The window states a compositor sets on a toplevel.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ToplevelStates {
    pub maximized: bool,
    pub fullscreen: bool,
    pub activated: bool,
    pub resizing: bool,
    pub tiled: bool,
    pub minimized: bool,
}

// The bits luminaria_rs.cpp packs ToplevelStates into.
const MAXIMIZED: u32 = 1;
const FULLSCREEN: u32 = 2;
const ACTIVATED: u32 = 4;
const RESIZING: u32 = 8;
const TILED: u32 = 16;
const MINIMIZED: u32 = 32;

impl ToplevelStates {
    fn from_bits(bits: u32) -> Self {
        Self {
            maximized: bits & MAXIMIZED != 0,
            fullscreen: bits & FULLSCREEN != 0,
            activated: bits & ACTIVATED != 0,
            resizing: bits & RESIZING != 0,
            tiled: bits & TILED != 0,
            minimized: bits & MINIMIZED != 0,
        }
    }
}

/// Where a popup's parent is drawn and the area it must stay inside, so a menu
/// near a screen edge flips or slides instead of hanging off it. Both boxes are
/// in layout coordinates.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PopupArea {
    pub parent: SurfaceId,
    pub parent_box: Rect,
    pub usable: Rect,
}

/// The `xdg_wm_base` (xdg-shell) global. Its methods take `&self` because
/// [`ForeignToplevelManager::track`](crate::desktop::ForeignToplevelManager::track)
/// keeps it borrowed.
pub struct XdgShell<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::XdgShellShim>,
    _display: Holds<'d>,
}

impl<'d> XdgShell<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::xdg_shell_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }
}

impl XdgShell<'_> {
    /// Answer maximize, fullscreen and minimize requests yourself, as
    /// [`ToplevelEvent::RequestMaximize`] and friends. Without this the library
    /// grants them, as a C++ compositor that listens to none of them gets.
    /// Affects toplevels created from now on, so call it before the first
    /// client connects.
    pub fn forward_window_requests(self) -> Self {
        self.raw.forward_window_requests();
        self
    }

    /// The area a maximized window may take (xdg_toplevel.configure_bounds).
    pub fn set_bounds(&self, width: i32, height: i32) {
        self.raw.set_bounds(width, height);
    }

    /// Every toplevel alive now, mapped or not, in no particular order.
    pub fn toplevels(&self) -> Vec<SurfaceId> {
        self.raw.toplevels().into_iter().map(SurfaceId).collect()
    }

    /// The toplevel whose surface is `surface`, if there is one now.
    pub fn toplevel(&self, surface: SurfaceId) -> Option<Toplevel<'_>> {
        let mut info = ffi::ToplevelInfoRaw::default();
        self.raw
            .toplevel_info(surface.0, &mut info)
            .then_some(Toplevel { shell: self.raw.as_ref()?, surface })
    }

    /// The popup whose surface is `surface`, if there is one now.
    pub fn popup(&self, surface: SurfaceId) -> Option<Popup<'_>> {
        let mut info = ffi::PopupInfoRaw::default();
        self.raw
            .popup_info(surface.0, &mut info)
            .then_some(Popup { shell: self.raw.as_ref()?, surface })
    }

    /// Replace the areas popups are kept inside. The shell asks while it places
    /// a popup, inside [`Display::dispatch`], and can only be answered from
    /// what it was told beforehand: update this whenever the scene moves. A
    /// popup whose parent is not listed is placed unconstrained.
    pub fn set_popup_constraint_areas(&self, areas: &[PopupArea]) {
        let raw: Vec<ffi::PopupAreaRaw> = areas
            .iter()
            .map(|a| ffi::PopupAreaRaw {
                surface_id: a.parent.0,
                parent: a.parent_box,
                usable: a.usable,
            })
            .collect();
        self.raw.set_popup_constraint_areas(&raw);
    }
}

/// A toplevel, looked up by its surface on every call. Once the client destroys
/// it the getters return defaults and the setters do nothing.
#[derive(Clone, Copy)]
pub struct Toplevel<'s> {
    shell: &'s ffi::XdgShellShim,
    surface: SurfaceId,
}

impl Toplevel<'_> {
    fn info(&self) -> ffi::ToplevelInfoRaw {
        let mut info = ffi::ToplevelInfoRaw::default();
        self.shell.toplevel_info(self.surface.0, &mut info);
        info
    }

    pub fn surface(&self) -> SurfaceId {
        self.surface
    }

    pub fn title(&self) -> String {
        self.shell.toplevel_title(self.surface.0)
    }

    pub fn app_id(&self) -> String {
        self.shell.toplevel_app_id(self.surface.0)
    }

    /// The toplevel this one is transient for.
    pub fn parent(&self) -> Option<SurfaceId> {
        SurfaceId::from_raw(self.info().parent)
    }

    pub fn is_mapped(&self) -> bool {
        self.info().mapped
    }

    /// The client's minimum size; 0 on an axis means no minimum.
    pub fn min_size(&self) -> (i32, i32) {
        let i = self.info();
        (i.min_width, i.min_height)
    }

    /// The client's maximum size; 0 on an axis means no maximum.
    pub fn max_size(&self) -> (i32, i32) {
        let i = self.info();
        (i.max_width, i.max_height)
    }

    /// The window geometry in surface coordinates: the part of the surface that
    /// is the window, without client-side shadows. The whole surface when the
    /// client set none.
    pub fn geometry(&self) -> Rect {
        self.info().geometry
    }

    /// The size last sent in a configure.
    pub fn pending_size(&self) -> (i32, i32) {
        let i = self.info();
        (i.pending_width, i.pending_height)
    }

    pub fn states(&self) -> ToplevelStates {
        ToplevelStates::from_bits(self.info().states)
    }

    /// Ask the client to take this size (0 = its choice) with the current
    /// states. Returns the configure serial.
    pub fn configure(&self, width: i32, height: i32) -> u32 {
        self.shell.toplevel_configure(self.surface.0, width, height)
    }

    // Each setter reconfigures the client and emits StateChange, but only when
    // the state actually changes.
    pub fn set_maximized(&self, on: bool) {
        self.shell.toplevel_set_state(self.surface.0, MAXIMIZED, on);
    }

    pub fn set_fullscreen(&self, on: bool) {
        self.shell.toplevel_set_state(self.surface.0, FULLSCREEN, on);
    }

    pub fn set_activated(&self, on: bool) {
        self.shell.toplevel_set_state(self.surface.0, ACTIVATED, on);
    }

    pub fn set_resizing(&self, on: bool) {
        self.shell.toplevel_set_state(self.surface.0, RESIZING, on);
    }

    pub fn set_tiled(&self, on: bool) {
        self.shell.toplevel_set_state(self.surface.0, TILED, on);
    }

    pub fn set_minimized(&self, on: bool) {
        self.shell.toplevel_set_state(self.surface.0, MINIMIZED, on);
    }

    /// Ask the client to close the window. It may refuse, or ask the user.
    pub fn close(&self) {
        self.shell.toplevel_close(self.surface.0);
    }
}

/// A popup, looked up by its surface on every call.
#[derive(Clone, Copy)]
pub struct Popup<'s> {
    shell: &'s ffi::XdgShellShim,
    surface: SurfaceId,
}

impl Popup<'_> {
    fn info(&self) -> ffi::PopupInfoRaw {
        let mut info = ffi::PopupInfoRaw::default();
        self.shell.popup_info(self.surface.0, &mut info);
        info
    }

    pub fn surface(&self) -> SurfaceId {
        self.surface
    }

    /// The surface it was created on, `None` for a layer surface's popup.
    pub fn parent(&self) -> Option<SurfaceId> {
        SurfaceId::from_raw(self.info().parent)
    }

    pub fn is_mapped(&self) -> bool {
        self.info().mapped
    }

    pub fn has_grab(&self) -> bool {
        self.info().has_grab
    }

    /// Where the positioner put it, relative to the parent's window geometry.
    pub fn geometry(&self) -> Rect {
        let i = self.info();
        Rect::new(i.x, i.y, i.width, i.height)
    }

    /// Close it, and every popup stacked on it (xdg_popup.popup_done).
    pub fn dismiss(&self) {
        self.shell.popup_dismiss(self.surface.0);
    }
}
