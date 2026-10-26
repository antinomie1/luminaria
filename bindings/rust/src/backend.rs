//! Display backends and input drivers: Output, Headless, Wayland, DRM, Libinput, Session.

use std::pin::Pin;

use crate::core::{Display, Error};
use crate::ffi;
use crate::util::{Color, Mode, Transform};

/// An active output surface (display or virtual window).
pub struct Output {
    pub(crate) raw: cxx::UniquePtr<ffi::OutputShim>,
}

impl Output {
    pub(crate) fn from_raw(raw: cxx::UniquePtr<ffi::OutputShim>) -> Self {
        Self { raw }
    }

    fn pin(&mut self) -> Pin<&mut ffi::OutputShim> {
        self.raw.pin_mut()
    }

    pub fn width(&self) -> i32 {
        self.raw.width()
    }

    pub fn height(&self) -> i32 {
        self.raw.height()
    }

    pub fn scale(&self) -> i32 {
        self.raw.scale()
    }

    pub fn set_scale(&mut self, scale: i32) {
        self.pin().set_scale(scale);
    }

    pub fn transform(&self) -> Transform {
        Transform::from_u32(self.raw.transform())
    }

    pub fn set_transform(&mut self, transform: Transform) {
        self.pin().set_transform(transform.as_u32());
    }

    pub fn logical_width(&self) -> i32 {
        self.raw.logical_width()
    }

    pub fn logical_height(&self) -> i32 {
        self.raw.logical_height()
    }

    pub fn schedule_frame(&mut self) {
        self.pin().schedule_frame();
    }

    pub fn frame_scheduled(&self) -> bool {
        self.raw.frame_scheduled()
    }

    pub fn modes(&self) -> Vec<Mode> {
        self.raw.modes()
    }

    pub fn current_mode(&self) -> Mode {
        self.raw.current_mode()
    }

    pub fn set_mode(&mut self, width: i32, height: i32, refresh_mhz: i32) -> Result<(), Error> {
        let mut err = String::new();
        if self.pin().set_mode(width, height, refresh_mhz, &mut err) {
            Ok(())
        } else {
            Err(Error(err))
        }
    }

    pub fn commit(&mut self, color: Color) -> Result<(), Error> {
        let mut err = String::new();
        if self
            .pin()
            .commit(color.r, color.g, color.b, color.a, &mut err)
        {
            Ok(())
        } else {
            Err(Error(err))
        }
    }

    pub fn commit_frame(
        &mut self,
        rgba: &[u8],
        width: i32,
        height: i32,
    ) -> Result<(), Error> {
        let mut err = String::new();
        if self.pin().commit_frame(rgba, width, height, &mut err) {
            Ok(())
        } else {
            Err(Error(err))
        }
    }

    pub fn scanout_modifiers(&self, drm_format: u32) -> Vec<u64> {
        self.raw.scanout_modifiers(drm_format)
    }

    pub fn has_cursor_plane(&self) -> bool {
        self.raw.has_cursor_plane()
    }

    pub fn move_cursor(&mut self, x: i32, y: i32) -> Result<(), Error> {
        let mut err = String::new();
        if self.pin().move_cursor(x, y, &mut err) {
            Ok(())
        } else {
            Err(Error(err))
        }
    }

    pub fn hide_cursor(&mut self) -> Result<(), Error> {
        let mut err = String::new();
        if self.pin().hide_cursor(&mut err) {
            Ok(())
        } else {
            Err(Error(err))
        }
    }

    /// Check if this output asked for a frame redraw, and reset the request flag.
    pub fn take_frame(&mut self) -> bool {
        self.pin().take_frame()
    }
}

pub use crate::util::Mode as OutputMode;

/// Headless software backend for tests and offscreen rendering.
pub struct HeadlessBackend {
    raw: cxx::UniquePtr<ffi::HeadlessBackendShim>,
}

impl HeadlessBackend {
    pub fn new(display: &Display, interval_ms: u32) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::headless_backend_new(&display.raw, interval_ms, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn add_output(&mut self, width: i32, height: i32) -> Result<Output, Error> {
        let raw_output = self.raw.pin_mut().add_output(width, height);
        if raw_output.is_null() {
            Err(Error("failed to create headless output".into()))
        } else {
            Ok(Output::from_raw(raw_output))
        }
    }

    pub fn start(&mut self) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.pin_mut().start(&mut err) {
            Ok(())
        } else {
            Err(Error(err))
        }
    }
}

/// Nested Wayland backend driving a host Wayland window.
pub struct WaylandBackend {
    raw: cxx::UniquePtr<ffi::WaylandBackendShim>,
}

impl WaylandBackend {
    pub fn new(display: &Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::wayland_backend_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn add_output(&mut self, width: i32, height: i32) -> Result<Output, Error> {
        let raw_output = self.raw.pin_mut().add_output(width, height);
        if raw_output.is_null() {
            Err(Error("failed to create wayland output".into()))
        } else {
            Ok(Output::from_raw(raw_output))
        }
    }

    pub fn start(&mut self) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.pin_mut().start(&mut err) {
            Ok(())
        } else {
            Err(Error(err))
        }
    }

    pub fn keymap(&self) -> String {
        self.raw.keymap()
    }

    /// Pull the next input event received from the host Wayland window.
    pub fn next_input_event(&mut self) -> Option<BackendInputEvent> {
        let mut raw = ffi::BackendInputEventRaw {
            kind: 0,
            code: 0,
            pressed: false,
            x: 0.0,
            y: 0.0,
        };
        if self.raw.pin_mut().next_input_event(&mut raw) {
            Some(match raw.kind {
                0 => BackendInputEvent::Key {
                    key: raw.code,
                    pressed: raw.pressed,
                },
                1 => BackendInputEvent::Motion { x: raw.x, y: raw.y },
                2 => BackendInputEvent::Button {
                    button: raw.code,
                    pressed: raw.pressed,
                },
                _ => return None,
            })
        } else {
            None
        }
    }
}

/// libseat session management for bare-metal KMS/DRM backends.
pub struct Session {
    pub(crate) raw: cxx::UniquePtr<ffi::SessionShim>,
}

impl Session {
    pub fn new(display: &Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::session_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn switch_to(&mut self, vt: i32) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.pin_mut().switch_to(vt, &mut err) {
            Ok(())
        } else {
            Err(Error(err))
        }
    }

    pub fn is_active(&self) -> bool {
        self.raw.active()
    }

    pub fn seat_name(&self) -> String {
        self.raw.seat_name()
    }
}

/// KMS/DRM backend for bare-metal display presentation.
pub struct DrmBackend {
    raw: cxx::UniquePtr<ffi::DrmBackendShim>,
}

impl DrmBackend {
    pub fn new(
        display: &Display,
        device: Option<&str>,
        session: Option<&Session>,
    ) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = if let Some(sess) = session {
            ffi::drm_backend_new(&display.raw, device.unwrap_or(""), &sess.raw, &mut err)
        } else {
            ffi::drm_backend_new_standalone(&display.raw, device.unwrap_or(""), &mut err)
        };
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn start(&mut self) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.pin_mut().start(&mut err) {
            Ok(())
        } else {
            Err(Error(err))
        }
    }
}

/// libinput input device manager for evdev keyboards, pointers, and touchscreens.
pub struct LibinputBackend {
    raw: cxx::UniquePtr<ffi::LibinputBackendShim>,
}

impl LibinputBackend {
    pub fn new(display: &Display, session: Option<&Session>) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = if let Some(sess) = session {
            ffi::libinput_backend_new(&display.raw, &sess.raw, &mut err)
        } else {
            ffi::libinput_backend_new_standalone(&display.raw, &mut err)
        };
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn start(&mut self) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.pin_mut().start(&mut err) {
            Ok(())
        } else {
            Err(Error(err))
        }
    }

    pub fn add_device(&mut self, path: &str) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.pin_mut().add_device(path, &mut err) {
            Ok(())
        } else {
            Err(Error(err))
        }
    }
}

/// An input event from a nested backend.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum BackendInputEvent {
    Key { key: u32, pressed: bool },
    Motion { x: f64, y: f64 },
    Button { button: u32, pressed: bool },
}

/// Raw input event types.
pub mod input_event {
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct KeyEvent {
        pub time_ms: u32,
        pub key: u32,
        pub pressed: bool,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    pub struct PointerMotionEvent {
        pub time_ms: u32,
        pub dx: f64,
        pub dy: f64,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct PointerButtonEvent {
        pub time_ms: u32,
        pub button: u32,
        pub pressed: bool,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    pub struct PointerAxisEvent {
        pub time_ms: u32,
        pub dx: f64,
        pub dy: f64,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    pub struct TouchDownEvent {
        pub time_ms: u32,
        pub id: i32,
        pub x: f64,
        pub y: f64,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    pub struct TouchMotionEvent {
        pub time_ms: u32,
        pub id: i32,
        pub x: f64,
        pub y: f64,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct TouchUpEvent {
        pub time_ms: u32,
        pub id: i32,
    }
}
