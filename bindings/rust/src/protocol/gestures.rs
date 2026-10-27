//! pointer-gestures: touchpad gestures (swipe, pinch, hold).

use super::SurfaceId;
use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi;

/// Touchpad pointer gestures protocol (`zwp_pointer_gestures_v1` v3).
pub struct PointerGestures<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::PointerGesturesShim>,
    _display: Holds<'d>,
}

impl<'d> PointerGestures<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::pointer_gestures_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                raw,
                _display: Holds::new(),
            })
        }
    }

    pub fn send_swipe_begin(&self, surface: SurfaceId, serial: u32, time_ms: u32, fingers: u32) {
        self.raw
            .send_swipe_begin(surface.0, serial, time_ms, fingers);
    }

    pub fn send_swipe_update(&self, time_ms: u32, dx: f64, dy: f64) {
        self.raw.send_swipe_update(time_ms, dx, dy);
    }

    pub fn send_swipe_end(&self, serial: u32, time_ms: u32, cancelled: bool) {
        self.raw.send_swipe_end(serial, time_ms, cancelled);
    }

    pub fn send_pinch_begin(&self, surface: SurfaceId, serial: u32, time_ms: u32, fingers: u32) {
        self.raw
            .send_pinch_begin(surface.0, serial, time_ms, fingers);
    }

    pub fn send_pinch_update(&self, time_ms: u32, dx: f64, dy: f64, scale: f64, rotation_deg: f64) {
        self.raw
            .send_pinch_update(time_ms, dx, dy, scale, rotation_deg);
    }

    pub fn send_pinch_end(&self, serial: u32, time_ms: u32, cancelled: bool) {
        self.raw.send_pinch_end(serial, time_ms, cancelled);
    }

    pub fn send_hold_begin(&self, surface: SurfaceId, serial: u32, time_ms: u32, fingers: u32) {
        self.raw
            .send_hold_begin(surface.0, serial, time_ms, fingers);
    }

    pub fn send_hold_end(&self, serial: u32, time_ms: u32, cancelled: bool) {
        self.raw.send_hold_end(serial, time_ms, cancelled);
    }
}
