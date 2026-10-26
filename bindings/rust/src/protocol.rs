//! Wayland protocol globals and managers: Compositor, XdgShell, Seat, LayerShell, etc.
//!
//! A global whose C++ type emits signals queues them, and its `next_event()`
//! hands them over one at a time: call it until it returns `None` after every
//! [`Display::dispatch`]. A queue nobody drains keeps growing. Objects clients
//! create (a toplevel, a layer surface, a text input) are named by their
//! surface's [`SurfaceId`] or by an id the binding assigns, never by a
//! reference; each lookup happens on the call, and a stale name finds nothing.

use crate::core::{Display, Error, Holds};
use crate::ffi;
use crate::util::{Rect, Transform};

mod activation;
mod idle;
mod layer_shell;
mod pointer_constraints;
mod seat;
mod selection;
mod text_input;
mod xdg_shell;

pub use activation::*;
pub use idle::*;
pub use layer_shell::*;
pub use pointer_constraints::*;
pub use seat::*;
pub use selection::*;
pub use text_input::*;
pub use xdg_shell::*;

/// Generational identity of a Wayland surface.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Default)]
pub struct SurfaceId(pub u64);

impl SurfaceId {
    pub const INVALID: Self = Self(0);

    /// `None` for the 0 the C++ side uses for "no surface".
    pub(crate) fn from_raw(raw: u64) -> Option<Self> {
        (raw != 0).then_some(Self(raw))
    }

    pub fn is_valid(&self) -> bool {
        self.0 != 0
    }

    pub fn surface_width(&self) -> i32 {
        ffi::surface_width(self.0)
    }

    pub fn surface_height(&self) -> i32 {
        ffi::surface_height(self.0)
    }

    pub fn buffer_width(&self) -> i32 {
        ffi::buffer_width(self.0)
    }

    pub fn buffer_height(&self) -> i32 {
        ffi::buffer_height(self.0)
    }

    pub fn buffer_scale(&self) -> i32 {
        ffi::buffer_scale(self.0)
    }

    pub fn buffer_transform(&self) -> Transform {
        Transform::from_u32(ffi::buffer_transform(self.0))
    }

    pub fn has_buffer(&self) -> bool {
        ffi::surface_has_buffer(self.0)
    }

    pub fn damage(&self) -> Vec<Rect> {
        ffi::surface_damage(self.0)
    }

    pub fn send_frame_done(&self, time_ms: u32) {
        ffi::surface_send_frame_done(self.0, time_ms);
    }

    pub fn notify_rendered(&self, fence_fd: i32) {
        ffi::surface_notify_rendered(self.0, fence_fd);
    }

    pub fn accepts_input(&self, sx: f64, sy: f64) -> bool {
        ffi::surface_accepts_input(self.0, sx, sy)
    }
}

/// A compositor event regarding surface lifecycle.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CompositorEvent {
    NewSurface(SurfaceId),
    Commit(SurfaceId),
    Destroy(SurfaceId),
    Rendered(SurfaceId),
    Invalidated(SurfaceId),
}

/// The `wl_compositor` global.
pub struct Compositor<'d> {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::CompositorShim>,
    _display: Holds<'d>,
}

impl<'d> Compositor<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::compositor_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    pub fn next_event(&self) -> Option<CompositorEvent> {
        loop {
            let raw = crate::next_raw_event(|out| self.raw.next_event(out))?;
            let surface = SurfaceId(raw.id);
            match raw.kind {
                ffi::EventKind::NewSurface => return Some(CompositorEvent::NewSurface(surface)),
                ffi::EventKind::SurfaceCommit => return Some(CompositorEvent::Commit(surface)),
                ffi::EventKind::SurfaceDestroy => return Some(CompositorEvent::Destroy(surface)),
                ffi::EventKind::SurfaceRendered => return Some(CompositorEvent::Rendered(surface)),
                ffi::EventKind::SurfaceInvalidated => return Some(CompositorEvent::Invalidated(surface)),
                _ => {}
            }
        }
    }
}

/// The `wl_subcompositor` global.
pub struct Subcompositor<'d> {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::SubcompositorShim>,
    _display: Holds<'d>,
}

impl<'d> Subcompositor<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::subcompositor_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }
}

/// The `wl_output` global publisher.
pub struct OutputGlobal<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::OutputGlobalShim>,
    _display: Holds<'d>,
}

impl<'d> OutputGlobal<'d> {
    pub fn new(
        display: &'d Display,
        width: i32,
        height: i32,
        name: &str,
    ) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::output_global_new(
            &display.raw,
            width,
            height,
            name,
            &mut err,
        );
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }
}

impl OutputGlobal<'_> {
    /// Where this output sits in the layout, as xdg-output reports it.
    pub fn set_logical_position(&self, x: i32, y: i32) {
        self.raw.set_logical_position(x, y);
    }

    /// The mode now driving the display; re-sent to every bound client.
    pub fn set_mode(&self, width: i32, height: i32, refresh_mhz: i32) {
        self.raw.set_mode(width, height, refresh_mhz);
    }

    pub fn set_scale(&self, scale: i32) {
        self.raw.set_scale(scale);
    }

    pub fn set_transform(&self, transform: Transform) {
        self.raw.set_transform(transform as u32);
    }
}

/// An Xcursor theme.
pub struct CursorTheme {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::CursorThemeShim>,
}

impl CursorTheme {
    pub fn load(name: &str, size: i32) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::cursor_theme_load(name, size, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

// ----------------------------------------------------------------------------
// Protocol Managers
// ----------------------------------------------------------------------------

macro_rules! define_manager {
    ($name:ident, $raw_type:ident, $constructor:ident) => {
        pub struct $name<'d> {
            #[allow(dead_code)]
            pub(crate) raw: cxx::UniquePtr<ffi::$raw_type>,
            _display: Holds<'d>,
        }

        impl<'d> $name<'d> {
            pub fn new(display: &'d Display) -> Result<Self, Error> {
                let mut err = String::new();
                let raw = ffi::$constructor(&display.raw, &mut err);
                if raw.is_null() {
                    Err(Error(err))
                } else {
                    Ok(Self {
                        raw,
                        _display: Holds::new(),
                    })
                }
            }
        }
    };
}

define_manager!(Viewporter, ViewporterShim, viewporter_new);
define_manager!(FractionalScaleManager, FractionalScaleManagerShim, fractional_scale_manager_new);
define_manager!(SinglePixelBufferManager, SinglePixelBufferManagerShim, single_pixel_buffer_manager_new);
define_manager!(TearingControlManager, TearingControlManagerShim, tearing_control_manager_new);
define_manager!(ContentTypeManager, ContentTypeManagerShim, content_type_manager_new);
define_manager!(FifoManager, FifoManagerShim, fifo_manager_new);
define_manager!(CommitTimingManager, CommitTimingManagerShim, commit_timing_manager_new);
define_manager!(Presentation, PresentationShim, presentation_new);
define_manager!(BackgroundEffectManager, BackgroundEffectManagerShim, background_effect_manager_new);

/// Window decoration modes (`zxdg_toplevel_decoration_v1.mode`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum DecorationMode {
    ClientSide = 1,
    ServerSide = 2,
}

impl DecorationMode {
    pub(crate) fn from_u32(val: u32) -> Option<Self> {
        match val {
            1 => Some(Self::ClientSide),
            2 => Some(Self::ServerSide),
            _ => None,
        }
    }
}

/// A client asks who decorates its window.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DecorationRequest {
    pub surface: Option<SurfaceId>,
    pub preferred: Option<DecorationMode>,
    pub mode: DecorationMode,
}

/// The `zxdg_decoration_manager_v1` global.
pub struct XdgDecorationManager<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::XdgDecorationManagerShim>,
    _display: Holds<'d>,
}

impl<'d> XdgDecorationManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::xdg_decoration_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    pub fn set_default_mode(&self, mode: DecorationMode) {
        self.raw.set_default_mode(mode as u32);
    }

    pub fn default_mode(&self) -> DecorationMode {
        DecorationMode::from_u32(self.raw.default_mode()).unwrap_or(DecorationMode::ClientSide)
    }

    pub fn next_event(&self) -> Option<DecorationRequest> {
        loop {
            let raw = crate::next_raw_event(|out| self.raw.next_event(out))?;
            match raw.kind {
                ffi::EventKind::DecorationRequest => {
                    return Some(DecorationRequest {
                        surface: SurfaceId::from_raw(raw.id),
                        preferred: DecorationMode::from_u32(raw.serial),
                        mode: DecorationMode::from_u32(raw.value).unwrap_or(DecorationMode::ClientSide),
                    });
                }
                _ => {}
            }
        }
    }
}

/// A request for a named cursor shape (`wp_cursor_shape_device_v1.set_shape`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CursorShapeRequest {
    pub serial: u32,
    pub shape: u32,
    pub name: String,
}

/// The `wp_cursor_shape_manager_v1` global.
pub struct CursorShapeManager<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::CursorShapeManagerShim>,
    _display: Holds<'d>,
}

impl<'d> CursorShapeManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::cursor_shape_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    pub fn next_event(&self) -> Option<CursorShapeRequest> {
        loop {
            let raw = crate::next_raw_event(|out| self.raw.next_event(out))?;
            match raw.kind {
                ffi::EventKind::CursorShapeRequest => {
                    return Some(CursorShapeRequest {
                        serial: raw.serial,
                        shape: raw.value,
                        name: raw.text,
                    });
                }
                _ => {}
            }
        }
    }
}

/// Notification that a toplevel's client-supplied icon changed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ToplevelIconChange {
    pub surface: SurfaceId,
    pub name: String,
}

/// The `xdg_toplevel_icon_manager_v1` global.
pub struct XdgToplevelIconManager<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::XdgToplevelIconManagerShim>,
    _display: Holds<'d>,
}

impl<'d> XdgToplevelIconManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::xdg_toplevel_icon_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    pub fn icon_name(&self, surface: SurfaceId) -> String {
        self.raw.icon_name(surface.0)
    }

    pub fn next_event(&self) -> Option<ToplevelIconChange> {
        loop {
            let raw = crate::next_raw_event(|out| self.raw.next_event(out))?;
            match raw.kind {
                ffi::EventKind::ToplevelIconChange => {
                    return Some(ToplevelIconChange {
                        surface: SurfaceId(raw.id),
                        name: raw.text,
                    });
                }
                _ => {}
            }
        }
    }
}

/// The `zwp_relative_pointer_manager_v1` global. It routes through the [`Seat`] it was created for,
/// which it keeps borrowed.
pub struct RelativePointerManager<'a> {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::RelativePointerManagerShim>,
    _borrows: Holds<'a>,
}

impl<'a> RelativePointerManager<'a> {
    pub fn new(display: &'a Display, seat: &'a Seat<'_>) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::relative_pointer_manager_new(&display.raw, &seat.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _borrows: Holds::new() })
        }
    }
}
