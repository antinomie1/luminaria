//! Display backends and input drivers: Output, Headless, Wayland, DRM, Libinput, Session.

use std::pin::Pin;

use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi;
use crate::util::{Color, DrmFourcc, Keysym, Mode, Transform};

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

    /// The refresh rate of [`Output::current_mode`] in hertz; 0 when the backend
    /// has no notion of one.
    pub fn refresh_hz(&self) -> f64 {
        f64::from(self.current_mode().refresh_mhz) / 1000.0
    }

    pub fn set_mode(&mut self, width: i32, height: i32, refresh_mhz: i32) -> Result<(), Error> {
        let mut err = String::new();
        if self.pin().set_mode(width, height, refresh_mhz, &mut err) {
            Ok(())
        } else {
            Err(Error::new(ErrorKind::Backend, err))
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
            Err(Error::new(ErrorKind::Backend, err))
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
            Err(Error::new(ErrorKind::Backend, err))
        }
    }

    pub fn scanout_modifiers(&self, format: DrmFourcc) -> Vec<u64> {
        self.raw.scanout_modifiers(format as u32)
    }

    pub fn has_cursor_plane(&self) -> bool {
        self.raw.has_cursor_plane()
    }

    pub fn move_cursor(&mut self, x: i32, y: i32) -> Result<(), Error> {
        let mut err = String::new();
        if self.pin().move_cursor(x, y, &mut err) {
            Ok(())
        } else {
            Err(Error::new(ErrorKind::Backend, err))
        }
    }

    pub fn hide_cursor(&mut self) -> Result<(), Error> {
        let mut err = String::new();
        if self.pin().hide_cursor(&mut err) {
            Ok(())
        } else {
            Err(Error::new(ErrorKind::Backend, err))
        }
    }

    /// False once the monitor behind this output is gone (unplugged). Every
    /// call is then a harmless no-op; drop it.
    pub fn is_alive(&self) -> bool {
        self.raw.alive()
    }

    /// This output's identity, which outlives it: what
    /// [`BackendEvent::OutputRemoved`] names. Never reused within a process.
    pub fn id(&self) -> OutputId {
        OutputId(self.raw.id())
    }
}

/// Frame presentation feedback timestamp and hardware timing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PresentEvent {
    pub tv_sec: u64,
    pub tv_nsec: u32,
    pub refresh_ns: u32,
    pub seq: u64,
    pub vsync: bool,
    pub hw_clock: bool,
}

impl PresentEvent {
    pub fn time_ms(&self) -> u32 {
        (self.tv_sec * 1000 + self.tv_nsec as u64 / 1_000_000) as u32
    }
}

impl std::fmt::Debug for Output {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Output")
            .field("id", &self.id())
            .field("alive", &self.is_alive())
            .field("size", &(self.width(), self.height()))
            .finish()
    }
}

/// Something that happened on one [`Output`]: [`crate::Event::Output`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OutputEvent {
    /// The output wants a frame, which `schedule_frame()` asked for: present
    /// one now. An idle output asks for none.
    Frame,
    /// A frame reached the screen.
    Present(PresentEvent),
    /// A `set_mode()` landed. The renderer, the layout and a following
    /// `OutputGlobal` have already caught up; only what the compositor sized
    /// for the old mode itself is left to it.
    ModeChanged,
}

/// Identifies an [`Output`], including after it was destroyed.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct OutputId(pub u64);

impl std::fmt::Display for OutputId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "output {}", self.0)
    }
}

/// What a backend reports about outputs: [`crate::Event::OutputAdded`] and [`crate::Event::OutputRemoved`] together.
#[derive(Debug)]
pub enum BackendEvent {
    /// An output was lit up: at `start()` for every one there is (nested and
    /// headless have theirs from the start), later on DRM hotplug.
    OutputAdded(Output),
    /// A monitor went away. The [`Output`] with this id is now inert; drop it,
    /// along with whatever was built for it. The layout and the renderer have
    /// already let go of it.
    OutputRemoved(OutputId),
}

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
            Err(Error::new(ErrorKind::Backend, err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    pub fn add_output(&mut self, width: i32, height: i32) -> Result<Output, Error> {
        let raw_output = self.raw.pin_mut().add_output(width, height);
        if raw_output.is_null() {
            Err(Error::new(ErrorKind::Backend, "failed to create headless output"))
        } else {
            Ok(Output::from_raw(raw_output))
        }
    }

    pub fn start(&mut self) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.pin_mut().start(&mut err) {
            Ok(())
        } else {
            Err(Error::new(ErrorKind::Backend, err))
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
            Err(Error::new(ErrorKind::Backend, err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    pub fn add_output(&mut self, width: i32, height: i32) -> Result<Output, Error> {
        let raw_output = self.raw.pin_mut().add_output(width, height);
        if raw_output.is_null() {
            Err(Error::new(ErrorKind::Backend, "failed to create wayland output"))
        } else {
            Ok(Output::from_raw(raw_output))
        }
    }

    pub fn start(&mut self) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.pin_mut().start(&mut err) {
            Ok(())
        } else {
            Err(Error::new(ErrorKind::Backend, err))
        }
    }

    /// The parent's keymap, which `InputEvent::Key` keysyms are read against;
    /// empty until it sends one. Hand it to `Seat::set_keymap()`.
    pub fn keymap(&self) -> String {
        self.raw.keymap()
    }

    /// What the parent's seat can do.
    pub fn capabilities(&self) -> InputCapabilities {
        InputCapabilities::from_bits(self.raw.capabilities())
    }

}

/// Input from the host window arrives as [`crate::Event::Input`]: absolute
/// motion in an output's coordinates, `PointerLeave` when the pointer leaves
/// our windows. The parent asking our window to close is
/// [`crate::Event::HostCloseRequest`].
impl InputSource for WaylandBackend<'_> {
    fn keymap(&self) -> String {
        WaylandBackend::keymap(self)
    }
    fn capabilities(&self) -> InputCapabilities {
        WaylandBackend::capabilities(self)
    }
    fn follow(&self, seat: &crate::protocol::Seat<'_>, touch: bool) {
        self.raw.follow_seat(&seat.raw, touch);
    }
}

// The bare-metal stack (libseat, KMS, libinput) lives in luminaria.gpu. The
// value types below stay in core: the nested backend reports them too.
#[cfg(feature = "gpu")]
mod bare_metal {
    use std::os::fd::{AsFd, AsRawFd, BorrowedFd, RawFd};

    use super::*;

    /// libseat session management for bare-metal KMS/DRM backends. The backends
    /// built on it keep it borrowed, which is why its methods take `&self`.
    /// One made by [`Session::new`] is owned; [`AutoBackend::session`] lends
    /// out the one the backend owns.
    pub struct Session<'d> {
        raw: SessionRaw<'d>,
        _display: Holds<'d>,
    }

    enum SessionRaw<'a> {
        Owned(cxx::UniquePtr<crate::ffi_gpu::SessionShim>),
        Lent(&'a crate::ffi_gpu::SessionShim),
    }

    impl<'d> Session<'d> {
        pub fn new(display: &'d Display) -> Result<Self, Error> {
            let mut err = String::new();
            let raw = crate::ffi_gpu::session_new(&display.raw, &mut err);
            if raw.is_null() {
                Err(Error::new(ErrorKind::Backend, err))
            } else {
                Ok(Self { raw: SessionRaw::Owned(raw), _display: Holds::new() })
            }
        }

        pub(crate) fn shim(&self) -> &crate::ffi_gpu::SessionShim {
            match &self.raw {
                SessionRaw::Owned(raw) => raw,
                SessionRaw::Lent(raw) => raw,
            }
        }

        pub fn switch_to(&self, vt: i32) -> Result<(), Error> {
            let mut err = String::new();
            if self.shim().switch_to(vt, &mut err) {
                Ok(())
            } else {
                Err(Error::new(ErrorKind::Backend, err))
            }
        }

        pub fn is_active(&self) -> bool {
            self.shim().active()
        }

        pub fn seat_name(&self) -> String {
            self.shim().seat_name()
        }

        /// The Ctrl+Alt+Fn binding: if `keysym` is one of XF86Switch_VT_1..12
        /// (what the keymap turns Ctrl+Alt+Fn into), switch to that VT and
        /// answer true. Release the seat's keys alongside — their releases
        /// arrive on the other VT. VT switches themselves arrive as
        /// [`crate::Event::SessionActive`].
        pub fn switch_vt_for(&self, keysym: crate::util::Keysym) -> bool {
            self.shim().switch_vt_for(keysym.raw())
        }
    }

    /// KMS/DRM backend for bare-metal display presentation. It borrows the
    /// [`Display`] (its page flips and hotplug monitor are event-loop sources) and
    /// the [`Session`] it opened the card through.
    pub struct DrmBackend<'a> {
        raw: cxx::UniquePtr<crate::ffi_gpu::DrmBackendShim>,
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
                crate::ffi_gpu::drm_backend_new(&display.raw, device.unwrap_or(""), sess.shim(), &mut err)
            } else {
                crate::ffi_gpu::drm_backend_new_standalone(&display.raw, device.unwrap_or(""), &mut err)
            };
            if raw.is_null() {
                Err(Error::new(ErrorKind::Backend, err))
            } else {
                Ok(Self { raw, _borrows: Holds::new() })
            }
        }

        /// Light up the connected monitors. Each arrives as a
        /// [`BackendEvent::OutputAdded`] after this returns.
        pub fn start(&mut self) -> Result<(), Error> {
            let mut err = String::new();
            if self.raw.pin_mut().start(&mut err) {
                Ok(())
            } else {
                Err(Error::new(ErrorKind::Backend, err))
            }
        }

        /// Re-read the connector list now. Done on every udev hotplug event once
        /// started; exposed for a manual poke.
        pub fn scan_connectors(&mut self) {
            self.raw.pin_mut().scan_connectors();
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

    /// libinput input device manager for evdev keyboards, pointers, and
    /// touchscreens. Borrows the [`Display`] and the [`Session`] that opens its
    /// devices, like [`DrmBackend`].
    pub struct LibinputBackend<'a> {
        raw: cxx::UniquePtr<crate::ffi_gpu::LibinputBackendShim>,
        _borrows: Holds<'a>,
    }

    impl<'a> LibinputBackend<'a> {
        /// Every input device udev reports on the seat, opened at `start()`.
        pub fn new(display: &'a Display, session: Option<&'a Session<'_>>) -> Result<Self, Error> {
            let mut err = String::new();
            let raw = if let Some(sess) = session {
                crate::ffi_gpu::libinput_backend_new(&display.raw, sess.shim(), &mut err)
            } else {
                crate::ffi_gpu::libinput_backend_new_standalone(&display.raw, &mut err)
            };
            if raw.is_null() {
                Err(Error::new(ErrorKind::Backend, err))
            } else {
                Ok(Self { raw, _borrows: Holds::new() })
            }
        }

        /// Exactly the devices handed to `add_device()`, and nothing udev finds.
        pub fn new_path(display: &'a Display, session: Option<&'a Session<'_>>) -> Result<Self, Error> {
            let mut err = String::new();
            let raw = if let Some(sess) = session {
                crate::ffi_gpu::libinput_backend_new_path(&display.raw, sess.shim(), &mut err)
            } else {
                crate::ffi_gpu::libinput_backend_new_path_standalone(&display.raw, &mut err)
            };
            if raw.is_null() {
                Err(Error::new(ErrorKind::Backend, err))
            } else {
                Ok(Self { raw, _borrows: Holds::new() })
            }
        }

        pub fn start(&mut self) -> Result<(), Error> {
            let mut err = String::new();
            if self.raw.pin_mut().start(&mut err) {
                Ok(())
            } else {
                Err(Error::new(ErrorKind::Backend, err))
            }
        }

        pub fn add_device(&mut self, path: &str) -> Result<(), Error> {
            let mut err = String::new();
            if self.raw.pin_mut().add_device(path, &mut err) {
                Ok(())
            } else {
                Err(Error::new(ErrorKind::Backend, err))
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

    }

    /// Relative motion, touch, and `Capabilities` as devices come and go, all
    /// as [`crate::Event::Input`].
    impl InputSource for LibinputBackend<'_> {
        fn keymap(&self) -> String {
            LibinputBackend::keymap(self)
        }
        fn capabilities(&self) -> InputCapabilities {
            LibinputBackend::capabilities(self)
        }
        fn follow(&self, seat: &crate::protocol::Seat<'_>, touch: bool) {
            self.raw.follow_seat(&seat.raw, touch);
        }
    }

    /// Which backend [`AutoBackend`] is asked for, and which one it picked.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
    pub enum BackendKind {
        /// Nested, then DRM when a libseat session can be had, then headless.
        /// Asked for, never picked.
        #[default]
        Auto,
        Nested,
        Drm,
        Headless,
    }

    impl BackendKind {
        /// "auto", "nested", "drm" or "headless": what `parse()` reads back.
        pub fn name(self) -> &'static str {
            match self {
                BackendKind::Auto => "auto",
                BackendKind::Nested => "nested",
                BackendKind::Drm => "drm",
                BackendKind::Headless => "headless",
            }
        }
    }

    impl std::fmt::Display for BackendKind {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            f.write_str(self.name())
        }
    }

    /// `"auto" | "nested" | "drm" | "headless"`, what `LUMINARIA_BACKEND` holds
    /// in the examples.
    impl std::str::FromStr for BackendKind {
        type Err = Error;

        fn from_str(text: &str) -> Result<Self, Error> {
            [BackendKind::Auto, BackendKind::Nested, BackendKind::Drm, BackendKind::Headless]
                .into_iter()
                .find(|k| k.name() == text)
                .ok_or_else(|| {
                    Error::new(
                        ErrorKind::InvalidArgument,
                        format!("backend \"{text}\": expected auto, nested, drm or headless"),
                    )
                })
        }
    }

    /// What [`AutoBackend::new`] is asked for.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct BackendOptions {
        pub kind: BackendKind,
        /// The DRM card, e.g. `/dev/dri/card1`; `None` takes the first one with
        /// a monitor.
        pub drm_device: Option<String>,
        /// The one output nested and headless come with. DRM's are its monitors.
        pub width: i32,
        pub height: i32,
        /// The nested window's title.
        pub title: String,
    }

    impl Default for BackendOptions {
        fn default() -> Self {
            Self {
                kind: BackendKind::Auto,
                drm_device: None,
                width: 1280,
                height: 800,
                title: "luminaria".into(),
            }
        }
    }

    /// The backend this machine can run, picked the way every compositor would
    /// pick it (C++ `AutoBackend`): nested when there is a parent compositor,
    /// DRM when a libseat session can be had, so ssh and CI never land on a VT,
    /// else headless. A kind asked for by name fails instead of falling back,
    /// and DRM then also runs without a session.
    ///
    /// It owns the session, the card and libinput, so nothing here borrows
    /// anything but the [`Display`]. Outputs arrive as [`BackendEvent`]s once it
    /// starts, input through [`AutoBackend::input`].
    pub struct AutoBackend<'d> {
        raw: cxx::UniquePtr<crate::ffi_gpu::AutoBackendShim>,
        _display: Holds<'d>,
    }

    impl<'d> AutoBackend<'d> {
        pub fn new(display: &'d Display, options: &BackendOptions) -> Result<Self, Error> {
            let kind = match options.kind {
                BackendKind::Auto => 0,
                BackendKind::Nested => 1,
                BackendKind::Drm => 2,
                BackendKind::Headless => 3,
            };
            let mut err = String::new();
            let raw = crate::ffi_gpu::auto_backend_new(
                &display.raw,
                kind,
                options.drm_device.as_deref().unwrap_or(""),
                options.width,
                options.height,
                &options.title,
                &mut err,
            );
            if raw.is_null() {
                Err(Error::new(ErrorKind::Backend, err))
            } else {
                Ok(Self { raw, _display: Holds::new() })
            }
        }

        /// What was picked; never `Auto`.
        pub fn kind(&self) -> BackendKind {
            match self.raw.kind() {
                1 => BackendKind::Nested,
                2 => BackendKind::Drm,
                _ => BackendKind::Headless,
            }
        }

        /// libinput, then the backend. Every output then arrives as a
        /// [`BackendEvent::OutputAdded`].
        pub fn start(&mut self) -> Result<(), Error> {
            let mut err = String::new();
            if self.raw.pin_mut().start(&mut err) {
                Ok(())
            } else {
                Err(Error::new(ErrorKind::Backend, err))
            }
        }

        /// Where input comes from: the host window nested, libinput on DRM;
        /// `None` headless. Its events arrive as [`crate::Event::Input`].
        pub fn input(&self) -> Option<&dyn InputSource> {
            if self.kind() == BackendKind::Headless {
                None
            } else {
                Some(self)
            }
        }

        /// The libseat session, lent out: VT switching. Only ever on DRM.
        /// Only ever on DRM.
        pub fn session(&self) -> Option<Session<'_>> {
            self.raw.has_session().then(|| Session {
                raw: SessionRaw::Lent(self.raw.session()),
                _display: Holds::new(),
            })
        }

        /// On DRM without a session (asked for by name, no seat to join): why.
        pub fn session_error(&self) -> Option<String> {
            let error = self.raw.session_error();
            (!error.is_empty()).then_some(error)
        }

        /// For `SceneRenderer::new`: on DRM the card, so the GPU that renders
        /// is the one behind the display.
        pub fn vulkan_options(&self) -> crate::gpu::VulkanOptions {
            let mut id = crate::gpu::DrmDeviceId { major: 0, minor: 0 };
            crate::gpu::VulkanOptions {
                drm_device: self.raw.drm_device(&mut id).then_some(id),
            }
        }
    }

    impl InputSource for AutoBackend<'_> {
        fn follow(&self, seat: &crate::protocol::Seat<'_>, touch: bool) {
            self.raw.follow_seat(&seat.raw, touch);
        }
        fn keymap(&self) -> String {
            self.raw.keymap()
        }
        fn capabilities(&self) -> InputCapabilities {
            InputCapabilities::from_bits(self.raw.capabilities())
        }
    }
}

#[cfg(feature = "gpu")]
pub use bare_metal::*;

/// xkb modifier masks plus the layout group, as `Seat::notify_modifiers()`
/// takes them.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Modifiers {
    pub depressed: u32,
    pub latched: u32,
    pub locked: u32,
    pub group: u32,
}

impl Modifiers {
    /// xkb's core modifier bits, for matching bindings against [`Modifiers::held`].
    pub const SHIFT: u32 = 1 << 0;
    pub const CTRL: u32 = 1 << 2;
    pub const ALT: u32 = 1 << 3;
    pub const LOGO: u32 = 1 << 6;
    /// The four a binding is made of. Caps and Num Lock are left out on
    /// purpose: a binding must not stop working because a lock is on.
    pub const BINDABLE: u32 = Self::SHIFT | Self::CTRL | Self::ALT | Self::LOGO;

    /// What is held down for a binding: pressed or latched, Shift, Ctrl, Alt
    /// and Logo only.
    pub const fn held(&self) -> u32 {
        (self.depressed | self.latched) & Self::BINDABLE
    }
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

/// One scroll frame: smooth deltas in surface units (positive is right/down),
/// wheel notches (0 on touchpads), and the end of a smooth sequence. Hand it
/// to `Seat::pointer_axis_event()`, which knows that a wheel reports both.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct PointerAxis {
    pub dx: f64,
    pub dy: f64,
    pub dx_steps: i32,
    pub dy_steps: i32,
    pub stop_horizontal: bool,
    pub stop_vertical: bool,
}

/// An input event from an [`InputSource`], either backend. Codes are evdev's,
/// which is what `Seat` forwards to clients.
#[derive(Debug, Clone, PartialEq)]
pub enum InputEvent {
    /// `keysym` and `modifiers` are the backend's reading of the key, against
    /// `InputSource::keymap()` and in the state from before the key: match
    /// bindings against them. Logo then Return is Return with Logo held.
    Key {
        key: u32,
        pressed: bool,
        keysym: Keysym,
        modifiers: Modifiers,
    },
    /// The masks changed. Forward to `Seat::notify_modifiers()`.
    Modifiers(Modifiers),
    /// The layout keys are now read against. Forward to `Seat::set_keymap()`.
    KeymapChange(String),
    /// Relative (libinput): the compositor owns where the pointer is.
    Motion { dx: f64, dy: f64 },
    /// Absolute (nested), in `output`'s coordinates; `None` if the window it
    /// is over was not one this backend handed out.
    MotionAbsolute { x: f64, y: f64, output: Option<OutputId> },
    /// The pointer left every output (nested: it left our windows).
    PointerLeave,
    Button { button: u32, pressed: bool },
    Axis(PointerAxis),
    /// Normalised to the touchscreen, (0,0) to (1,1); mapping onto an output
    /// is the compositor's.
    TouchDown { id: i32, x: f64, y: f64 },
    TouchMotion { id: i32, x: f64, y: f64 },
    TouchUp { id: i32 },
    /// Close the burst with `Seat::touch_frame()`.
    TouchFrame,
    TouchCancel { id: i32 },
    /// What the devices can do changed. Forward to `Seat::set_capabilities()`.
    Capabilities(InputCapabilities),
}

impl InputEvent {
    pub(crate) fn empty_raw() -> ffi::InputEventRaw {
        ffi::InputEventRaw {
            kind: ffi::InputEventKind::Key,
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
            output: 0,
            text: String::new(),
        }
    }

    pub(crate) fn from_raw(raw: ffi::InputEventRaw) -> Self {
        use ffi::InputEventKind as K;
        let modifiers = Modifiers {
            depressed: raw.depressed,
            latched: raw.latched,
            locked: raw.locked,
            group: raw.group,
        };
        match raw.kind {
            K::Key => InputEvent::Key {
                key: raw.code,
                pressed: raw.pressed,
                keysym: Keysym::new(raw.keysym),
                modifiers,
            },
            K::Modifiers => InputEvent::Modifiers(modifiers),
            K::KeymapChange => InputEvent::KeymapChange(raw.text),
            K::Motion => InputEvent::Motion { dx: raw.x, dy: raw.y },
            K::MotionAbsolute => InputEvent::MotionAbsolute {
                x: raw.x,
                y: raw.y,
                output: (raw.output != 0).then_some(OutputId(raw.output)),
            },
            K::PointerLeave => InputEvent::PointerLeave,
            K::Button => InputEvent::Button {
                button: raw.code,
                pressed: raw.pressed,
            },
            K::Axis => InputEvent::Axis(PointerAxis {
                dx: raw.x,
                dy: raw.y,
                dx_steps: raw.steps_x,
                dy_steps: raw.steps_y,
                stop_horizontal: raw.flags & 1 != 0,
                stop_vertical: raw.flags & 2 != 0,
            }),
            K::TouchDown => InputEvent::TouchDown {
                id: raw.id,
                x: raw.x,
                y: raw.y,
            },
            K::TouchMotion => InputEvent::TouchMotion {
                id: raw.id,
                x: raw.x,
                y: raw.y,
            },
            K::TouchUp => InputEvent::TouchUp { id: raw.id },
            K::TouchFrame => InputEvent::TouchFrame,
            K::TouchCancel => InputEvent::TouchCancel { id: raw.id },
            _ => InputEvent::Capabilities(InputCapabilities::from_bits(raw.flags)),
        }
    }
}

/// Where input comes from: [`LibinputBackend`] on bare metal,
/// [`WaylandBackend`] nested. What differs between them is which
/// [`InputEvent`]s arrive as [`crate::Event::Input`], not how, so a compositor
/// handles whichever it has in one match. Hand it to `Seat::follow()` and the
/// seat describes its devices to clients by itself.
///
/// Sealed: only luminaria's backends are input sources.
pub trait InputSource: sealed::Sealed {
    /// The keymap `InputEvent::Key` is read against; empty while there is
    /// none yet. Later changes arrive as `InputEvent::KeymapChange`.
    fn keymap(&self) -> String;
    /// What the devices can do now. Later changes arrive as
    /// `InputEvent::Capabilities`.
    fn capabilities(&self) -> InputCapabilities;
    #[doc(hidden)]
    fn follow(&self, seat: &crate::protocol::Seat<'_>, touch: bool);
}

mod sealed {
    pub trait Sealed {}
    impl Sealed for super::WaylandBackend<'_> {}
    #[cfg(feature = "gpu")]
    impl Sealed for super::LibinputBackend<'_> {}
    #[cfg(feature = "gpu")]
    impl Sealed for super::AutoBackend<'_> {}
}
