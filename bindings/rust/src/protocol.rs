//! Wayland protocol globals and managers: Compositor, XdgShell, Seat, LayerShell, etc.

use std::pin::Pin;

use crate::core::{Display, Error};
use crate::ffi;
use crate::util::{Rect, Transform};

/// Generational identity of a Wayland surface.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Default)]
pub struct SurfaceId(pub u64);

impl SurfaceId {
    pub const INVALID: Self = Self(0);

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
pub struct Compositor {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::CompositorShim>,
}

impl Compositor {
    pub fn new(display: &Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::compositor_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

/// The `wl_subcompositor` global.
pub struct Subcompositor {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::SubcompositorShim>,
}

impl Subcompositor {
    pub fn new(display: &Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::subcompositor_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

/// An event reported by an XDG toplevel.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ToplevelEvent {
    Created(SurfaceId),
    Map(SurfaceId),
    Unmap(SurfaceId),
    Destroy(SurfaceId),
}

/// The `xdg_wm_base` (xdg-shell) global.
pub struct XdgShell {
    pub(crate) raw: cxx::UniquePtr<ffi::XdgShellShim>,
}

impl XdgShell {
    pub fn new(display: &Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::xdg_shell_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn set_bounds(&mut self, width: i32, height: i32) {
        self.raw.pin_mut().set_bounds(width, height);
    }

    /// Pull the next toplevel lifecycle event.
    pub fn next_event(&mut self) -> Option<ToplevelEvent> {
        let mut raw = ffi::ToplevelEventRaw {
            kind: 0,
            surface_id: 0,
        };
        if self.raw.pin_mut().next_toplevel_event(&mut raw) {
            let surface = SurfaceId(raw.surface_id);
            Some(match raw.kind {
                0 => ToplevelEvent::Created(surface),
                1 => ToplevelEvent::Map(surface),
                2 => ToplevelEvent::Unmap(surface),
                3 => ToplevelEvent::Destroy(surface),
                _ => return None,
            })
        } else {
            None
        }
    }

    pub fn configure(&mut self, surface: SurfaceId, width: i32, height: i32) {
        self.raw
            .pin_mut()
            .toplevel_configure(surface.0, width, height);
    }

    pub fn set_maximized(&mut self, surface: SurfaceId, on: bool) {
        self.raw.pin_mut().toplevel_set_maximized(surface.0, on);
    }

    pub fn set_activated(&mut self, surface: SurfaceId, on: bool) {
        self.raw.pin_mut().toplevel_set_activated(surface.0, on);
    }

    pub fn close(&mut self, surface: SurfaceId) {
        self.raw.pin_mut().toplevel_close(surface.0);
    }
}

/// The `wl_seat` global.
pub struct Seat {
    pub(crate) raw: cxx::UniquePtr<ffi::SeatShim>,
}

impl Seat {
    pub fn new(display: &Display, name: &str) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::seat_new(&display.raw, name, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    fn pin(&mut self) -> Pin<&mut ffi::SeatShim> {
        self.raw.pin_mut()
    }

    pub fn set_capabilities(&mut self, keyboard: bool, pointer: bool, touch: bool) {
        self.pin().set_capabilities(keyboard, pointer, touch);
    }

    pub fn set_keymap(&mut self, xkb_text: &str) -> bool {
        self.pin().set_keymap(xkb_text)
    }

    pub fn keymap(&self) -> String {
        self.raw.keymap()
    }

    pub fn set_keyboard_focus(&mut self, surface: Option<SurfaceId>) {
        self.pin().set_keyboard_focus(surface.map_or(0, |s| s.0));
    }

    pub fn keyboard_focus(&self) -> Option<SurfaceId> {
        match self.raw.keyboard_focus() {
            0 => None,
            id => Some(SurfaceId(id)),
        }
    }

    pub fn notify_key(&mut self, key: u32, pressed: bool) {
        self.pin().notify_key(key, pressed);
    }

    pub fn notify_modifiers(&mut self, depressed: u32, latched: u32, locked: u32, group: u32) {
        self.pin()
            .notify_modifiers(depressed, latched, locked, group);
    }

    pub fn pointer_enter(&mut self, surface: SurfaceId, sx: f64, sy: f64) {
        self.pin().pointer_enter(surface.0, sx, sy);
    }

    pub fn pointer_clear_focus(&mut self) {
        self.pin().pointer_clear_focus();
    }

    pub fn pointer_focus(&self) -> Option<SurfaceId> {
        match self.raw.pointer_focus() {
            0 => None,
            id => Some(SurfaceId(id)),
        }
    }

    pub fn pointer_grab(&self) -> Option<SurfaceId> {
        match self.raw.pointer_grab() {
            0 => None,
            id => Some(SurfaceId(id)),
        }
    }

    pub fn pointer_motion(&mut self, sx: f64, sy: f64) {
        self.pin().pointer_motion(sx, sy);
    }

    pub fn pointer_button(&mut self, button: u32, pressed: bool) {
        self.pin().pointer_button(button, pressed);
    }

    pub fn pointer_axis(&mut self, dx: f64, dy: f64) {
        self.pin().pointer_axis(dx, dy);
    }

    pub fn pointer_axis_discrete(&mut self, dx_steps: i32, dy_steps: i32) {
        self.pin().pointer_axis_discrete(dx_steps, dy_steps);
    }

    pub fn pointer_axis_stop(&mut self, horizontal: bool, vertical: bool) {
        self.pin().pointer_axis_stop(horizontal, vertical);
    }

    pub fn cursor_surface(&self) -> Option<SurfaceId> {
        match self.raw.cursor_surface() {
            0 => None,
            id => Some(SurfaceId(id)),
        }
    }

    pub fn cursor_hidden(&self) -> bool {
        self.raw.cursor_hidden()
    }

    pub fn cursor_hotspot(&self) -> (i32, i32) {
        (self.raw.cursor_hotspot_x(), self.raw.cursor_hotspot_y())
    }

    pub fn touch_down(&mut self, surface: SurfaceId, id: i32, x: f64, y: f64) {
        self.pin().touch_down(surface.0, id, x, y);
    }

    pub fn touch_motion(&mut self, id: i32, x: f64, y: f64) {
        self.pin().touch_motion(id, x, y);
    }

    pub fn touch_up(&mut self, id: i32) {
        self.pin().touch_up(id);
    }

    pub fn touch_frame(&mut self) {
        self.pin().touch_frame();
    }

    pub fn touch_cancel(&mut self) {
        self.pin().touch_cancel();
    }

    pub fn touch_focus(&self, id: i32) -> Option<SurfaceId> {
        match self.raw.touch_focus(id) {
            0 => None,
            surface => Some(SurfaceId(surface)),
        }
    }
}

/// The `zwlr_layer_shell_v1` global.
pub struct LayerShell {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::LayerShellShim>,
}

impl LayerShell {
    pub fn new(display: &Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::layer_shell_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

/// The `wl_output` global publisher.
pub struct OutputGlobal {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::OutputGlobalShim>,
}

impl OutputGlobal {
    pub fn new(
        display: &Display,
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
            Ok(Self { raw })
        }
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
        pub struct $name {
            #[allow(dead_code)]
            pub(crate) raw: cxx::UniquePtr<ffi::$raw_type>,
        }

        impl $name {
            pub fn new(display: &Display) -> Result<Self, Error> {
                let mut err = String::new();
                let raw = ffi::$constructor(&display.raw, &mut err);
                if raw.is_null() {
                    Err(Error(err))
                } else {
                    Ok(Self { raw })
                }
            }
        }
    };
}

define_manager!(Viewporter, ViewporterShim, viewporter_new);
define_manager!(FractionalScaleManager, FractionalScaleManagerShim, fractional_scale_manager_new);
define_manager!(XdgActivation, XdgActivationShim, xdg_activation_new);
define_manager!(XdgDecorationManager, XdgDecorationManagerShim, xdg_decoration_manager_new);
define_manager!(CursorShapeManager, CursorShapeManagerShim, cursor_shape_manager_new);
define_manager!(IdleInhibitManager, IdleInhibitManagerShim, idle_inhibit_manager_new);
define_manager!(SinglePixelBufferManager, SinglePixelBufferManagerShim, single_pixel_buffer_manager_new);
define_manager!(TearingControlManager, TearingControlManagerShim, tearing_control_manager_new);
define_manager!(ContentTypeManager, ContentTypeManagerShim, content_type_manager_new);
define_manager!(FifoManager, FifoManagerShim, fifo_manager_new);
define_manager!(CommitTimingManager, CommitTimingManagerShim, commit_timing_manager_new);
define_manager!(Presentation, PresentationShim, presentation_new);
define_manager!(BackgroundEffectManager, BackgroundEffectManagerShim, background_effect_manager_new);
define_manager!(XdgToplevelIconManager, XdgToplevelIconManagerShim, xdg_toplevel_icon_manager_new);

pub struct TextInputManager {
    pub(crate) raw: cxx::UniquePtr<ffi::TextInputManagerShim>,
}

impl TextInputManager {
    pub fn new(display: &Display, seat: &Seat) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::text_input_manager_new(&display.raw, &seat.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

pub struct IdleNotifier {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::IdleNotifierShim>,
}

impl IdleNotifier {
    pub fn new(display: &Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::idle_notifier_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

pub struct DataDeviceManager {
    pub(crate) raw: cxx::UniquePtr<ffi::DataDeviceManagerShim>,
}

impl DataDeviceManager {
    pub fn new(display: &Display, seat: &Seat) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::data_device_manager_new(&display.raw, &seat.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

pub struct PrimarySelectionManager {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::PrimarySelectionManagerShim>,
}

impl PrimarySelectionManager {
    pub fn new(display: &Display, seat: &Seat) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::primary_selection_manager_new(&display.raw, &seat.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

pub struct RelativePointerManager {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::RelativePointerManagerShim>,
}

impl RelativePointerManager {
    pub fn new(display: &Display, seat: &Seat) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::relative_pointer_manager_new(&display.raw, &seat.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

pub struct PointerConstraints {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::PointerConstraintsShim>,
}

impl PointerConstraints {
    pub fn new(display: &Display, seat: &Seat) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::pointer_constraints_new(&display.raw, &seat.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}
