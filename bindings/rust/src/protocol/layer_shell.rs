//! wlr-layer-shell: panels, docks, wallpapers, notifications, lock screens.

use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi::{self, EventKind};
use crate::util::Rect;

use super::{OutputGlobal, SurfaceId};

/// z-depth bands, bottom-most first. Application windows live between
/// `Bottom` and `Top`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Default)]
pub enum Layer {
    #[default]
    Background = 0,
    Bottom = 1,
    Top = 2,
    Overlay = 3,
}

impl Layer {
    fn from_u32(v: u32) -> Self {
        match v {
            1 => Self::Bottom,
            2 => Self::Top,
            3 => Self::Overlay,
            _ => Self::Background,
        }
    }
}

/// Whether a layer surface may hold keyboard focus.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum KeyboardInteractivity {
    /// Never takes focus.
    #[default]
    None = 0,
    /// Takes all keyboard input while mapped: a lock screen, a launcher.
    Exclusive = 1,
    /// Can be clicked into, like a window.
    OnDemand = 2,
}

/// Space a layer surface keeps from the edges it is anchored to.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Margins {
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
    pub left: i32,
}

/// Something a layer surface did.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LayerSurfaceEvent {
    /// The surface took the layer role. It maps after its first configure,
    /// which the compositor sends ([`LayerSurface::configure`],
    /// [`LayerSurface::arrange`] or [`LayerManager::arrange`](crate::shell::LayerManager::arrange)).
    Created,
    Map,
    Unmap,
    Destroy,
    /// A committed change of layer, anchor, size, margin or exclusive zone:
    /// lay the output out again.
    StateChange,
    /// The client anchored this xdg popup (by its surface) to the layer
    /// surface. Its [`Popup::parent`](super::Popup::parent) is `None`; this is
    /// the only place that says what it belongs to.
    NewPopup(SurfaceId),
}

/// The `zwlr_layer_shell_v1` global.
pub struct LayerShell<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::LayerShellShim>,
    _display: Holds<'d>,
}

impl<'d> LayerShell<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::layer_shell_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }
}

impl LayerShell<'_> {

    /// Every layer surface alive now, in no particular order.
    pub fn layer_surfaces(&self) -> Vec<SurfaceId> {
        self.raw.layer_surfaces().into_iter().map(SurfaceId).collect()
    }

    /// The layer surface whose surface is `surface`, if there is one now.
    pub fn layer_surface(&self, surface: SurfaceId) -> Option<LayerSurface<'_>> {
        let mut info = ffi::LayerSurfaceInfoRaw::default();
        self.raw
            .info(surface.0, &mut info)
            .then_some(LayerSurface { shell: self.raw.as_ref()?, surface })
    }
}

/// A layer surface, looked up by its surface on every call. The getters report
/// the client's last committed state.
#[derive(Clone, Copy)]
pub struct LayerSurface<'s> {
    shell: &'s ffi::LayerShellShim,
    surface: SurfaceId,
}

impl LayerSurface<'_> {
    fn info(&self) -> ffi::LayerSurfaceInfoRaw {
        let mut info = ffi::LayerSurfaceInfoRaw::default();
        self.shell.info(self.surface.0, &mut info);
        info
    }

    pub fn surface(&self) -> SurfaceId {
        self.surface
    }

    /// The client's stated purpose ("panel", "notifications", ...); the
    /// protocol calls it `namespace`.
    pub fn scope(&self) -> String {
        self.shell.scope(self.surface.0)
    }

    pub fn layer(&self) -> Layer {
        Layer::from_u32(self.info().layer)
    }

    /// The edges it sticks to, as a [`crate::util::edge`] bitmask.
    pub fn anchor(&self) -> u32 {
        self.info().anchor
    }

    /// The size it asked for; 0 on an axis means "the compositor decides".
    pub fn desired_size(&self) -> (i32, i32) {
        let i = self.info();
        (i.desired_width, i.desired_height)
    }

    /// The strip along its anchored edge others must not cover. 0: none, but
    /// keep out of other surfaces' zones; -1: ignore them.
    pub fn exclusive_zone(&self) -> i32 {
        self.info().exclusive_zone
    }

    /// The edge the exclusive zone applies to when the anchors are ambiguous;
    /// 0 when they decide it.
    pub fn exclusive_edge(&self) -> u32 {
        self.info().exclusive_edge
    }

    pub fn margins(&self) -> Margins {
        let i = self.info();
        Margins {
            top: i.margin_top,
            right: i.margin_right,
            bottom: i.margin_bottom,
            left: i.margin_left,
        }
    }

    pub fn keyboard_interactivity(&self) -> KeyboardInteractivity {
        match self.info().keyboard_interactivity {
            1 => KeyboardInteractivity::Exclusive,
            2 => KeyboardInteractivity::OnDemand,
            _ => KeyboardInteractivity::None,
        }
    }

    pub fn is_mapped(&self) -> bool {
        self.info().mapped
    }

    /// The size last sent in a configure (0 before the first).
    pub fn pending_size(&self) -> (i32, i32) {
        let i = self.info();
        (i.pending_width, i.pending_height)
    }

    /// Whether the client named an output, rather than leaving the choice to
    /// the compositor.
    pub fn has_output(&self) -> bool {
        self.info().has_output
    }

    /// Whether the output the client named is `output`.
    pub fn is_on(&self, output: &OutputGlobal<'_>) -> bool {
        self.shell.on_output(self.surface.0, &output.raw)
    }

    /// Ask the client to take this size (0 = its choice). Returns the
    /// configure serial; the same size twice is not re-sent.
    pub fn configure(&self, width: i32, height: i32) -> u32 {
        self.shell.configure(self.surface.0, width, height)
    }

    /// Tell the client the surface is gone for good (its output went away).
    pub fn close(&self) {
        self.shell.close(self.surface.0);
    }

    /// Lay it out on an output whose whole box is `full` and configure it.
    /// `usable` is what earlier layer surfaces left, and shrinks by this one's
    /// exclusive zone. Returns where to draw it. [`LayerManager`](crate::shell::LayerManager)
    /// does this for every surface of an output, in the right order.
    pub fn arrange(&self, full: Rect, usable: &mut Rect) -> Rect {
        self.shell.arrange(self.surface.0, full, usable)
    }
}

impl LayerSurfaceEvent {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<(SurfaceId, LayerSurfaceEvent)> {
        let event = match raw.kind {
            EventKind::NewLayerSurface => LayerSurfaceEvent::Created,
            EventKind::LayerSurfaceMap => LayerSurfaceEvent::Map,
            EventKind::LayerSurfaceUnmap => LayerSurfaceEvent::Unmap,
            EventKind::LayerSurfaceDestroy => LayerSurfaceEvent::Destroy,
            EventKind::LayerSurfaceStateChange => LayerSurfaceEvent::StateChange,
            EventKind::LayerSurfacePopup => LayerSurfaceEvent::NewPopup(SurfaceId(raw.other)),
            _ => return None,
        };
        return Some((SurfaceId(raw.id), event));
        None
    }
}
