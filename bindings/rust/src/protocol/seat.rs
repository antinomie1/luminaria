//! wl_seat: keyboard, pointer and touch routing, and what focus did.

use crate::backend::{Modifiers, PointerAxis};
use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi::{self, EventKind};

use super::SurfaceId;

/// A change in the seat's routing, whoever caused it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SeatEvent {
    /// The keyboard went to this surface, or to none.
    KeyboardFocus(Option<SurfaceId>),
    /// The pointer entered this surface, or left them all.
    PointerFocus(Option<SurfaceId>),
    /// The pointer-focused client set its cursor: a surface with a hotspot,
    /// or `hidden` when it asked for none.
    CursorChange {
        surface: Option<SurfaceId>,
        hotspot: (i32, i32),
        hidden: bool,
    },
}

/// The `wl_seat` global.
///
/// [`Cursor`](crate::gpu::Cursor) and the seat-bound managers
/// ([`DataDeviceManager`](super::DataDeviceManager), [`TextInputManager`](super::TextInputManager), ...) keep it borrowed while
/// they live — the C++ objects hold a `Seat&` — so its methods take `&self`.
pub struct Seat<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::SeatShim>,
    _display: Holds<'d>,
}

impl<'d> Seat<'d> {
    pub fn new(display: &'d Display, name: &str) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::seat_new(&display.raw, name, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    pub fn set_capabilities(&self, keyboard: bool, pointer: bool, touch: bool) {
        self.raw.set_capabilities(keyboard, pointer, touch);
    }

    pub fn set_keymap(&self, xkb_text: &str) -> bool {
        self.raw.set_keymap(xkb_text)
    }

    pub fn keymap(&self) -> String {
        self.raw.keymap()
    }

    pub fn set_keyboard_focus(&self, surface: Option<SurfaceId>) {
        self.raw.set_keyboard_focus(surface.map_or(0, |s| s.0));
    }

    pub fn keyboard_focus(&self) -> Option<SurfaceId> {
        match self.raw.keyboard_focus() {
            0 => None,
            id => Some(SurfaceId(id)),
        }
    }

    pub fn notify_key(&self, key: u32, pressed: bool) {
        self.raw.notify_key(key, pressed);
    }

    pub fn notify_modifiers(&self, modifiers: Modifiers) {
        let Modifiers { depressed, latched, locked, group } = modifiers;
        self.raw.notify_modifiers(depressed, latched, locked, group);
    }

    /// Release every key a client was told is down, and clear the modifiers:
    /// for when the keyboard stops reaching this seat (a VT switch), and the
    /// releases themselves never will.
    pub fn release_keys(&self) {
        self.raw.release_keys();
    }

    /// Describe `source`'s devices to clients from now on: its keymap and
    /// capabilities at once, and every later change. Touch is advertised only
    /// with `touch`, for a compositor that routes it. Events are not routed:
    /// keys and the pointer are still the compositor's to deliver.
    pub fn follow(&self, source: &dyn crate::backend::InputSource, touch: bool) {
        source.follow(self, touch);
    }

    pub fn pointer_enter(&self, surface: SurfaceId, sx: f64, sy: f64) {
        self.raw.pointer_enter(surface.0, sx, sy);
    }

    pub fn pointer_clear_focus(&self) {
        self.raw.pointer_clear_focus();
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

    pub fn pointer_motion(&self, sx: f64, sy: f64) {
        self.raw.pointer_motion(sx, sy);
    }

    pub fn pointer_button(&self, button: u32, pressed: bool) {
        self.raw.pointer_button(button, pressed);
    }

    pub fn pointer_axis(&self, dx: f64, dy: f64) {
        self.raw.pointer_axis(dx, dy);
    }

    pub fn pointer_axis_discrete(&self, dx_steps: i32, dy_steps: i32) {
        self.raw.pointer_axis_discrete(dx_steps, dy_steps);
    }

    pub fn pointer_axis_stop(&self, horizontal: bool, vertical: bool) {
        self.raw.pointer_axis_stop(horizontal, vertical);
    }

    /// One backend scroll frame, as the three calls above. A wheel reports its
    /// notches AND their smooth value; the notches win, because
    /// `pointer_axis_discrete` already sends the value that goes with them and
    /// sending the smooth one as well scrolls every notch twice.
    pub fn pointer_axis_event(&self, axis: PointerAxis) {
        self.raw.pointer_axis_event(
            axis.dx,
            axis.dy,
            axis.dx_steps,
            axis.dy_steps,
            axis.stop_horizontal,
            axis.stop_vertical,
        );
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

    pub fn touch_down(&self, surface: SurfaceId, id: i32, x: f64, y: f64) {
        self.raw.touch_down(surface.0, id, x, y);
    }

    pub fn touch_motion(&self, id: i32, x: f64, y: f64) {
        self.raw.touch_motion(id, x, y);
    }

    pub fn touch_up(&self, id: i32) {
        self.raw.touch_up(id);
    }

    pub fn touch_frame(&self) {
        self.raw.touch_frame();
    }

    pub fn touch_cancel(&self) {
        self.raw.touch_cancel();
    }

    pub fn touch_focus(&self, id: i32) -> Option<SurfaceId> {
        match self.raw.touch_focus(id) {
            0 => None,
            surface => Some(SurfaceId(surface)),
        }
    }
}

impl SeatEvent {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<SeatEvent> {
        let surface = SurfaceId::from_raw(raw.id);
        match raw.kind {
            EventKind::SeatKeyboardFocus => return Some(SeatEvent::KeyboardFocus(surface)),
            EventKind::SeatPointerFocus => return Some(SeatEvent::PointerFocus(surface)),
            EventKind::SeatCursorChange => {
                return Some(SeatEvent::CursorChange {
                    surface,
                    hotspot: (raw.x, raw.y),
                    hidden: raw.flag,
                });
            }
            _ => {}
        }
        None
    }
}
