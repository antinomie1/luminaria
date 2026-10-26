//! Display backends and input drivers: Output, Headless, Wayland, DRM, Libinput, Session.

use std::os::fd::{AsFd, AsRawFd, BorrowedFd, RawFd};
use std::pin::Pin;

use crate::core::{Display, Error, Holds};
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

    /// True once after a `set_mode()` has landed. Everything sized for the old
    /// mode has to be rebuilt then: `SceneRenderer::reset()`, the layout box,
    /// the `OutputGlobal`.
    pub fn take_mode_changed(&mut self) -> bool {
        self.pin().take_mode_changed()
    }

    /// False once the monitor behind this output is gone (unplugged). Every
    /// call is then a harmless no-op; drop it.
    pub fn is_alive(&self) -> bool {
        self.raw.alive()
    }

    /// This output's identity, which outlives it: what
    /// [`DrmEvent::OutputRemoved`] names. Never reused within a process.
    pub fn id(&self) -> OutputId {
        OutputId(self.raw.id())
    }
}

/// Identifies an [`Output`], including after it was destroyed.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct OutputId(pub u64);

pub use crate::util::Mode as OutputMode;

/// Headless software backend for tests and offscreen rendering. Its timer is a
/// source on the display's event loop, so it borrows the [`Display`].
pub struct HeadlessBackend<'d> {
    raw: cxx::UniquePtr<ffi::HeadlessBackendShim>,
    _display: Holds<'d>,
}

impl<'d> HeadlessBackend<'d> {
    pub fn new(display: &'d Display, interval_ms: u32) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::headless_backend_new(&display.raw, interval_ms, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
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

/// Nested Wayland backend driving a host Wayland window. The host connection
/// is a source on the display's event loop, so it borrows the [`Display`].
pub struct WaylandBackend<'d> {
    raw: cxx::UniquePtr<ffi::WaylandBackendShim>,
    _display: Holds<'d>,
}

impl<'d> WaylandBackend<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::wayland_backend_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
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

/// libseat session management for bare-metal KMS/DRM backends. The backends
/// built on it keep it borrowed, which is why its methods take `&self`.
pub struct Session<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::SessionShim>,
    _display: Holds<'d>,
}

impl<'d> Session<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::session_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    pub fn switch_to(&self, vt: i32) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.switch_to(vt, &mut err) {
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

    /// The next VT switch, oldest first: `Some(false)` when the session was
    /// taken away, `Some(true)` when it came back. On the way out, release every
    /// key and button a client was told is down; on the way back, repaint.
    pub fn next_activity(&self) -> Option<bool> {
        match self.raw.next_activity() {
            1 => Some(false),
            2 => Some(true),
            _ => None,
        }
    }
}

/// KMS/DRM backend for bare-metal display presentation. It borrows the
/// [`Display`] (its page flips and hotplug monitor are event-loop sources) and
/// the [`Session`] it opened the card through.
pub struct DrmBackend<'a> {
    raw: cxx::UniquePtr<ffi::DrmBackendShim>,
    _borrows: Holds<'a>,
}

impl<'a> DrmBackend<'a> {
    pub fn new(
        display: &'a Display,
        device: Option<&str>,
        session: Option<&'a Session<'_>>,
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
            Ok(Self { raw, _borrows: Holds::new() })
        }
    }

    /// Light up the connected monitors. Each arrives as a
    /// [`DrmEvent::OutputAdded`] after this returns.
    pub fn start(&mut self) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.pin_mut().start(&mut err) {
            Ok(())
        } else {
            Err(Error(err))
        }
    }

    /// Re-read the connector list now. Done on every udev hotplug event once
    /// started; exposed for a manual poke.
    pub fn scan_connectors(&mut self) {
        self.raw.pin_mut().scan_connectors();
    }

    /// The next monitor that appeared or went away, oldest first. Drain it
    /// after every `Display::dispatch()`.
    pub fn next_event(&mut self) -> Option<DrmEvent> {
        let mut kind = 0;
        let mut removed = 0;
        let output = self.raw.pin_mut().next_event(&mut kind, &mut removed);
        match kind {
            1 => Some(DrmEvent::OutputAdded(Output::from_raw(output))),
            2 => Some(DrmEvent::OutputRemoved(OutputId(removed))),
            _ => None,
        }
    }
}

/// The card's fd, borrowed for the life of the backend. Query it (the `drm`
/// crate), derive the renderer's GPU from it (`gpu::DrmDeviceId::from_fd`), or
/// dup it for `gbm`; never close it, and never modeset or atomic-commit
/// through it — the KMS state is luminaria's.
impl AsFd for DrmBackend<'_> {
    fn as_fd(&self) -> BorrowedFd<'_> {
        // SAFETY: the backend owns the fd and keeps it open until it drops,
        // which the borrow on `self` outlasts.
        unsafe { BorrowedFd::borrow_raw(self.raw.fd()) }
    }
}

impl AsRawFd for DrmBackend<'_> {
    fn as_raw_fd(&self) -> RawFd {
        self.raw.fd()
    }
}

/// What a [`DrmBackend`] reports about monitors.
pub enum DrmEvent {
    /// A monitor was lit up: at `start()` for every connected one, later on
    /// hotplug.
    OutputAdded(Output),
    /// A monitor went away. The [`Output`] with this id is now inert; drop it,
    /// along with whatever was built for it.
    OutputRemoved(OutputId),
}

/// libinput input device manager for evdev keyboards, pointers, and
/// touchscreens. Borrows the [`Display`] and the [`Session`] that opens its
/// devices, like [`DrmBackend`].
pub struct LibinputBackend<'a> {
    raw: cxx::UniquePtr<ffi::LibinputBackendShim>,
    _borrows: Holds<'a>,
}

impl<'a> LibinputBackend<'a> {
    /// Every input device udev reports on the seat, opened at `start()`.
    pub fn new(display: &'a Display, session: Option<&'a Session<'_>>) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = if let Some(sess) = session {
            ffi::libinput_backend_new(&display.raw, &sess.raw, &mut err)
        } else {
            ffi::libinput_backend_new_standalone(&display.raw, &mut err)
        };
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _borrows: Holds::new() })
        }
    }

    /// Exactly the devices handed to `add_device()`, and nothing udev finds.
    pub fn new_path(display: &'a Display, session: Option<&'a Session<'_>>) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = if let Some(sess) = session {
            ffi::libinput_backend_new_path(&display.raw, &sess.raw, &mut err)
        } else {
            ffi::libinput_backend_new_path_standalone(&display.raw, &mut err)
        };
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _borrows: Holds::new() })
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

    /// The layout modifier masks are computed against. Hand the same text to
    /// `Seat::set_keymap()`.
    pub fn keymap(&self) -> String {
        self.raw.keymap()
    }

    /// Replace the layout; false (and the old one kept) if it does not compile.
    pub fn set_keymap(&mut self, xkb_text: &str) -> bool {
        self.raw.pin_mut().set_keymap(xkb_text)
    }

    /// What the open devices can do. Empty until `start()`.
    pub fn capabilities(&self) -> InputCapabilities {
        InputCapabilities::from_bits(self.raw.capabilities())
    }

    /// The next input event, oldest first. Drain it after every
    /// `Display::dispatch()`.
    pub fn next_input_event(&mut self) -> Option<LibinputEvent> {
        let mut raw = ffi::LibinputEventRaw {
            kind: 0,
            code: 0,
            keysym: 0,
            pressed: false,
            x: 0.0,
            y: 0.0,
            steps_x: 0,
            steps_y: 0,
            id: 0,
            flags: 0,
            depressed: 0,
            latched: 0,
            locked: 0,
            group: 0,
        };
        if !self.raw.pin_mut().next_input_event(&mut raw) {
            return None;
        }
        let modifiers = Modifiers {
            depressed: raw.depressed,
            latched: raw.latched,
            locked: raw.locked,
            group: raw.group,
        };
        Some(match raw.kind {
            0 => LibinputEvent::Key {
                key: raw.code,
                pressed: raw.pressed,
                keysym: raw.keysym,
                modifiers,
            },
            1 => LibinputEvent::Modifiers(modifiers),
            2 => LibinputEvent::Motion { dx: raw.x, dy: raw.y },
            3 => LibinputEvent::Button {
                button: raw.code,
                pressed: raw.pressed,
            },
            4 => LibinputEvent::Axis {
                dx: raw.x,
                dy: raw.y,
                dx_steps: raw.steps_x,
                dy_steps: raw.steps_y,
                stop_horizontal: raw.flags & 1 != 0,
                stop_vertical: raw.flags & 2 != 0,
            },
            5 => LibinputEvent::TouchDown {
                id: raw.id,
                x: raw.x,
                y: raw.y,
            },
            6 => LibinputEvent::TouchMotion {
                id: raw.id,
                x: raw.x,
                y: raw.y,
            },
            7 => LibinputEvent::TouchUp { id: raw.id },
            8 => LibinputEvent::TouchFrame,
            9 => LibinputEvent::TouchCancel { id: raw.id },
            _ => LibinputEvent::Capabilities(InputCapabilities::from_bits(raw.flags)),
        })
    }
}

/// xkb modifier masks plus the layout group, as `Seat::notify_modifiers()`
/// takes them.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Modifiers {
    pub depressed: u32,
    pub latched: u32,
    pub locked: u32,
    pub group: u32,
}

/// What kinds of input device are open. Hand it to `Seat::set_capabilities()`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct InputCapabilities {
    pub keyboard: bool,
    pub pointer: bool,
    pub touch: bool,
}

impl InputCapabilities {
    fn from_bits(bits: u32) -> Self {
        Self {
            keyboard: bits & 1 != 0,
            pointer: bits & 2 != 0,
            touch: bits & 4 != 0,
        }
    }
}

/// An input event from [`LibinputBackend`]. Codes are evdev's, which is what
/// `Seat` forwards to clients.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum LibinputEvent {
    /// `keysym` and `modifiers` are the keyboard state as this key arrived,
    /// before it was applied: match bindings against them rather than a state
    /// read later, which later keys of the same batch have moved on.
    Key {
        key: u32,
        pressed: bool,
        keysym: u32,
        modifiers: Modifiers,
    },
    /// The masks after a key, only when they changed. Forward to
    /// `Seat::notify_modifiers()`.
    Modifiers(Modifiers),
    /// Relative: the compositor owns where the pointer is.
    Motion { dx: f64, dy: f64 },
    Button { button: u32, pressed: bool },
    /// One scroll frame: smooth deltas, wheel notches (0 on touchpads), and
    /// the end of a smooth sequence.
    Axis {
        dx: f64,
        dy: f64,
        dx_steps: i32,
        dy_steps: i32,
        stop_horizontal: bool,
        stop_vertical: bool,
    },
    /// Normalised to the touchscreen, (0,0) to (1,1); mapping onto an output
    /// is the compositor's.
    TouchDown { id: i32, x: f64, y: f64 },
    TouchMotion { id: i32, x: f64, y: f64 },
    TouchUp { id: i32 },
    /// Close the burst with `Seat::touch_frame()`.
    TouchFrame,
    TouchCancel { id: i32 },
    /// A device came or went and what the seat can do changed.
    Capabilities(InputCapabilities),
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
