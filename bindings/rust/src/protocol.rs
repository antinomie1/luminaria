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

    pub fn accepts_input(&self, sx: f64, sy: f64) -> bool {
        ffi::surface_accepts_input(self.0, sx, sy)
    }
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
define_manager!(XdgDecorationManager, XdgDecorationManagerShim, xdg_decoration_manager_new);
define_manager!(CursorShapeManager, CursorShapeManagerShim, cursor_shape_manager_new);
define_manager!(SinglePixelBufferManager, SinglePixelBufferManagerShim, single_pixel_buffer_manager_new);
define_manager!(TearingControlManager, TearingControlManagerShim, tearing_control_manager_new);
define_manager!(ContentTypeManager, ContentTypeManagerShim, content_type_manager_new);
define_manager!(FifoManager, FifoManagerShim, fifo_manager_new);
define_manager!(CommitTimingManager, CommitTimingManagerShim, commit_timing_manager_new);
define_manager!(Presentation, PresentationShim, presentation_new);
define_manager!(BackgroundEffectManager, BackgroundEffectManagerShim, background_effect_manager_new);
define_manager!(XdgToplevelIconManager, XdgToplevelIconManagerShim, xdg_toplevel_icon_manager_new);

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
