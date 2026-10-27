//! Wayland protocol globals and managers: Compositor, XdgShell, Seat, LayerShell, etc.
//!
//! What a global reports arrives through [`Display::dispatch`] as a
//! [`crate::Event`]. Objects clients create (a toplevel, a layer surface, a
//! text input) are named by their surface's [`SurfaceId`] or by an id the
//! binding assigns, never by a reference; each lookup happens on the call, and
//! a stale name finds nothing.

mod core_globals;
pub use core_globals::CoreGlobals;

use crate::core::{Display, Error, ErrorKind, Holds};
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

    /// The surface tree rooted here, back to front, subsurfaces included. Each
    /// entry's x/y is its offset from this surface, so input that belongs to a
    /// grabbed surface can be sent in that surface's own coordinates even when
    /// it is a subsurface: add the offset, subtract it again on the way back.
    pub fn surface_tree(&self) -> Vec<SurfaceAt> {
        ffi::surface_tree(self.0)
            .into_iter()
            .filter_map(|at| {
                let surface = Self::from_raw(at.surface_id)?;
                Some(SurfaceAt { surface, x: at.x, y: at.y })
            })
            .collect()
    }

    /// Render at `scale` device pixels per logical pixel. Ignored for clients
    /// that negotiated an older `wl_surface` version.
    pub fn set_preferred_buffer_scale(&self, scale: i32) {
        ffi::surface_set_preferred_buffer_scale(self.0, scale);
    }

    /// A hint for clients that rotate their buffers themselves.
    pub fn set_preferred_buffer_transform(&self, transform: Transform) {
        ffi::surface_set_preferred_buffer_transform(self.0, transform.as_u32());
    }
}

/// One surface of a [`SurfaceId::surface_tree`], at its offset from the root.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SurfaceAt {
    pub surface: SurfaceId,
    pub x: i32,
    pub y: i32,
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
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
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
            Err(Error::new(ErrorKind::Protocol, err))
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
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    /// Advertise `output` as `layout` places it, and keep following both:
    /// a mode change re-sends the mode, scale and transform, a layout change
    /// the position. An empty `name` picks `luminaria-<n>`. This is the one a
    /// compositor wants; [`OutputGlobal::new`] is for an output that is not a
    /// backend's.
    pub fn for_output(
        display: &'d Display,
        output: &crate::backend::Output,
        layout: &crate::shell::OutputLayout,
        name: &str,
    ) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::output_global_for(&display.raw, &output.raw, &layout.raw, name, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
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

    /// Current logical position in global layout coordinates.
    pub fn logical_position(&self) -> (i32, i32) {
        (self.raw.logical_x(), self.raw.logical_y())
    }

    /// Size in layout coordinates.
    pub fn logical_size(&self) -> (i32, i32) {
        (self.raw.logical_width(), self.raw.logical_height())
    }

    /// Physical mode resolution (width, height).
    pub fn size(&self) -> (i32, i32) {
        (self.raw.width(), self.raw.height())
    }

    /// Physical width in pixels.
    pub fn width(&self) -> i32 {
        self.raw.width()
    }

    /// Physical height in pixels.
    pub fn height(&self) -> i32 {
        self.raw.height()
    }

    /// Logical width in layout coordinates.
    pub fn logical_width(&self) -> i32 {
        self.raw.logical_width()
    }

    /// Logical height in layout coordinates.
    pub fn logical_height(&self) -> i32 {
        self.raw.logical_height()
    }

    /// Integer output scale factor.
    pub fn scale(&self) -> i32 {
        self.raw.scale()
    }

    /// Output rotation/reflection transform.
    pub fn transform(&self) -> Transform {
        Transform::from_u32(self.raw.transform())
    }

    /// Output refresh rate in milli-Hertz (e.g. 60000 for 60Hz).
    pub fn refresh_mhz(&self) -> i32 {
        self.raw.refresh_mhz()
    }

    /// The name of this output (e.g. "eDP-1", "HEADLESS-1").
    pub fn name(&self) -> String {
        self.raw.name()
    }
}

/// An Xcursor theme.
#[cfg(feature = "shell")]
pub struct CursorTheme {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<crate::ffi_shell::CursorThemeShim>,
}

#[cfg(feature = "shell")]
impl CursorTheme {
    pub fn load(name: &str, size: i32) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = crate::ffi_shell::cursor_theme_load(name, size, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
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
                    Err(Error::new(ErrorKind::Protocol, err))
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
define_manager!(SinglePixelBufferManager, SinglePixelBufferManagerShim, single_pixel_buffer_manager_new);
define_manager!(TearingControlManager, TearingControlManagerShim, tearing_control_manager_new);
define_manager!(ContentTypeManager, ContentTypeManagerShim, content_type_manager_new);
define_manager!(FifoManager, FifoManagerShim, fifo_manager_new);
define_manager!(CommitTimingManager, CommitTimingManagerShim, commit_timing_manager_new);
define_manager!(Presentation, PresentationShim, presentation_new);
define_manager!(BackgroundEffectManager, BackgroundEffectManagerShim, background_effect_manager_new);

/// The `wp_fractional_scale_manager_v1` global.
///
/// Scale is per surface and per output: [tell](Self::set_scale) it when a
/// window lands on an output, and again when that output's scale changes.
pub struct FractionalScaleManager<'d> {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::FractionalScaleManagerShim>,
    _display: Holds<'d>,
}

impl<'d> FractionalScaleManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::fractional_scale_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    /// Tell `surface` the scale of the output it is on, e.g. 1.5, in both
    /// currencies: the exact value to a client that bound
    /// `wp_fractional_scale_v1`, and the integer preferred-buffer-scale hint
    /// rounded up to every other one.
    pub fn set_scale(&self, surface: SurfaceId, scale: f64) {
        self.raw.set_scale(surface.0, scale);
    }
}

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
            Err(Error::new(ErrorKind::Protocol, err))
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
    // Read by `gpu::Cursor`; its requests arrive as `Event::CursorShape`.
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::CursorShapeManagerShim>,
    _display: Holds<'d>,
}

impl<'d> CursorShapeManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::cursor_shape_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
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
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    pub fn icon_name(&self, surface: SurfaceId) -> String {
        self.raw.icon_name(surface.0)
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
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self { raw, _borrows: Holds::new() })
        }
    }
}

impl CompositorEvent {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<CompositorEvent> {
        let surface = SurfaceId(raw.id);
        match raw.kind {
            ffi::EventKind::NewSurface => return Some(CompositorEvent::NewSurface(surface)),
            ffi::EventKind::SurfaceCommit => return Some(CompositorEvent::Commit(surface)),
            ffi::EventKind::SurfaceDestroy => return Some(CompositorEvent::Destroy(surface)),
            ffi::EventKind::SurfaceRendered => return Some(CompositorEvent::Rendered(surface)),
            ffi::EventKind::SurfaceInvalidated => return Some(CompositorEvent::Invalidated(surface)),
            _ => {}
        }
        None
    }
}

impl DecorationRequest {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<DecorationRequest> {
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
        None
    }
}

impl CursorShapeRequest {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<CursorShapeRequest> {
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
        None
    }
}

impl ToplevelIconChange {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<ToplevelIconChange> {
        match raw.kind {
            ffi::EventKind::ToplevelIconChange => {
                return Some(ToplevelIconChange {
                    surface: SurfaceId(raw.id),
                    name: raw.text,
                });
            }
            _ => {}
        }
        None
    }
}
