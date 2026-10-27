//! Core library primitives: Display, EventLoop, errors, and input routers.

use std::collections::HashSet;
use std::ffi::c_void;
use std::fmt;
use std::marker::PhantomData;
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, RawFd};
use std::time::Duration;

use crate::backend::InputEvent;
use crate::event::Events;
use crate::ffi;
pub use crate::protocol::PointerGestures;
use crate::protocol::Seat;

/// Category of error originating from luminaria operations.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum ErrorKind {
    /// Failure during initialization of core objects (display, socket, shm).
    Init,
    /// Failure in event loop dispatch or source handling.
    EventLoop,
    /// Backend failure (DRM modesetting, libinput, session, headless, nested).
    Backend,
    /// GPU or Vulkan rendering failure.
    Gpu,
    /// Wayland protocol global creation or request processing failure.
    Protocol,
    /// Shell layout, compositing, or tree failure.
    Shell,
    /// Invalid arguments, bounds, or buffer dimensions.
    InvalidArgument,
    /// Other unclassified or internal error.
    Other,
}

impl fmt::Display for ErrorKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Init => write!(f, "initialization error"),
            Self::EventLoop => write!(f, "event loop error"),
            Self::Backend => write!(f, "backend error"),
            Self::Gpu => write!(f, "gpu error"),
            Self::Protocol => write!(f, "protocol error"),
            Self::Shell => write!(f, "shell error"),
            Self::InvalidArgument => write!(f, "invalid argument"),
            Self::Other => write!(f, "other error"),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    pub(crate) kind: ErrorKind,
    pub(crate) message: String,
}

impl Error {
    pub fn new(kind: ErrorKind, message: impl Into<String>) -> Self {
        Self {
            kind,
            message: message.into(),
        }
    }

    pub fn kind(&self) -> ErrorKind {
        self.kind
    }

    pub fn message(&self) -> &str {
        &self.message
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for Error {}

impl From<String> for Error {
    fn from(s: String) -> Self {
        Self {
            kind: ErrorKind::Other,
            message: s,
        }
    }
}

impl From<&str> for Error {
    fn from(s: &str) -> Self {
        Self {
            kind: ErrorKind::Other,
            message: s.to_owned(),
        }
    }
}

/// What a wrapper keeps borrowed for as long as it lives: the C++ object behind
/// it holds a reference or pointer to whatever `'a` was taken from.
///
/// The `Drop` impl is the point. Without one, dropck lets a value outlive the
/// borrow in its type as long as nothing *uses* it afterwards, and the implicit
/// drop at the end of a scope does not count as a use — so `drop(seat)` before
/// a `Cursor` that holds the C++ `Seat&` would compile. With it, the borrow has
/// to last until the wrapper is dropped.
pub(crate) struct Holds<'a>(PhantomData<&'a ()>);

impl Holds<'_> {
    pub(crate) fn new() -> Self {
        Self(PhantomData)
    }
}

impl Drop for Holds<'_> {
    fn drop(&mut self) {}
}

/// The Wayland display server context. Owns `wl_display` and the main event loop.
pub struct Display {
    pub(crate) raw: cxx::UniquePtr<ffi::DisplayShim>,
}

impl Display {
    pub fn new() -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::display_new(&mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Init, err))
        } else {
            Ok(Self { raw })
        }
    }

    /// Auto-bind a socket (`wayland-0`, `wayland-1`, ...) and return its name.
    pub fn add_socket_auto(&self) -> Result<String, Error> {
        let mut err = String::new();
        let name = self.raw.add_socket_auto(&mut err);
        if name.is_empty() {
            Err(Error::new(ErrorKind::Init, err))
        } else {
            Ok(name)
        }
    }

    /// Bind an explicitly named socket.
    pub fn add_socket(&self, name: &str) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.add_socket(name, &mut err) {
            Ok(())
        } else {
            Err(Error::new(ErrorKind::Init, err))
        }
    }

    /// Initialize built-in `wl_shm` support.
    pub fn init_shm(&self) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.init_shm(&mut err) {
            Ok(())
        } else {
            Err(Error::new(ErrorKind::Init, err))
        }
    }

    /// Run the main loop until `terminate` is called.
    pub fn run(&self) {
        self.raw.run();
    }

    /// Stop the main loop.
    pub fn terminate(&self) {
        self.raw.terminate();
    }

    /// One turn of the compositor: send the clients what the last turn
    /// produced, wait up to `timeout` (`None`: until something happens) for
    /// the event loop, dispatch it, and hand back every [`Event`] that left.
    ///
    /// Everything any wrapper on this display forwards comes back here, oldest
    /// first, so there is no queue of its own to forget to drain.
    pub fn dispatch(&self, timeout: Option<Duration>) -> Result<Events, Error> {
        let ms = timeout.map_or(-1, |t| t.as_millis().min(i32::MAX as u128) as i32);
        let ret = self.raw.dispatch(ms);
        if ret < 0 {
            Err(Error::new(
                ErrorKind::EventLoop,
                format!("event loop dispatch error: {ret}"),
            ))
        } else {
            Ok(self.events())
        }
    }

    /// Send the clients what is pending now, instead of at the next dispatch.
    pub fn flush_clients(&self) {
        self.raw.flush_clients();
    }

    /// The `wl_display` itself, for registering globals luminaria does not
    /// provide through libwayland-server's C API (`wl_global_create`,
    /// `wl_resource_create`, ...): `wayland-sys` with its `server` feature, or
    /// your own `extern "C"` block.
    ///
    /// Getting the pointer is safe; everything done with it is `unsafe`, and
    /// the contract is libwayland's plus luminaria's teardown order:
    ///
    /// - It is valid until this `Display` drops. Never `wl_display_destroy` it.
    /// - A global you create must die before the `Display`: destroy it from the
    ///   `Drop` of something declared *after* the `Display`.
    /// - Your client resources outlive your global, because still-connected
    ///   clients are destroyed inside the `Display`'s drop. Their destroy
    ///   handlers may touch only what the resource itself owns.
    /// - Your callbacks run inside [`Display::dispatch`]: do not panic out of
    ///   them, and do not call back into luminaria's wrappers from there.
    /// - Leave `wl_display_set_global_filter` alone: luminaria's data-control
    ///   filter owns the display's one slot.
    ///
    /// `wayland-server`'s own `Display<D>` cannot adopt this pointer: its
    /// backend only creates displays, it never wraps a foreign one.
    /// See `docs/api.md` §9.1 and `tests/test_raw_display.rs`.
    pub fn as_ptr(&self) -> *mut c_void {
        self.raw.raw_display() as *mut c_void
    }

    /// Get a non-owning view of the event loop, valid while this `Display` is.
    pub fn event_loop(&self) -> EventLoop<'_> {
        EventLoop {
            fd: self.raw.fd(),
            _display: PhantomData,
        }
    }
}

impl AsRawFd for Display {
    fn as_raw_fd(&self) -> RawFd {
        self.raw.fd()
    }
}

impl AsFd for Display {
    fn as_fd(&self) -> BorrowedFd<'_> {
        unsafe { BorrowedFd::borrow_raw(self.raw.fd()) }
    }
}

/// A non-owning handle to the event loop. It borrows the [`Display`] that owns
/// the loop, so its fd cannot be used after the loop is gone.
#[derive(Clone, Copy)]
pub struct EventLoop<'d> {
    fd: RawFd,
    _display: PhantomData<&'d Display>,
}

impl EventLoop<'_> {
    pub fn fd(&self) -> RawFd {
        self.fd
    }
}

impl AsRawFd for EventLoop<'_> {
    fn as_raw_fd(&self) -> RawFd {
        self.fd
    }
}

impl AsFd for EventLoop<'_> {
    fn as_fd(&self) -> BorrowedFd<'_> {
        // SAFETY: the Display owns the loop fd and keeps it open until it
        // drops, which the `'d` borrow of it outlasts.
        unsafe { BorrowedFd::borrow_raw(self.fd) }
    }
}

/// Splits the keyboard between the compositor's bindings and the focused
/// client, keeping each key whole: a key whose press a binding took has its
/// release taken too, so the client never sees half of it.
#[derive(Debug, Default)]
pub struct KeyRouter {
    kept: HashSet<u32>,
}

impl KeyRouter {
    pub fn new() -> Self {
        Self::default()
    }

    /// Route one [`InputEvent::Key`]; anything else is ignored. A press is
    /// offered to `binding` first, which answers whether the compositor keeps
    /// it; everything else goes to `seat`.
    ///
    /// A press of a key a binding is thought to still hold means its release
    /// was lost — what happens to the key that switched the VT away — and is
    /// treated as a fresh press.
    pub fn route(
        &mut self,
        seat: &Seat<'_>,
        event: &InputEvent,
        binding: impl FnOnce(&InputEvent) -> bool,
    ) {
        let &InputEvent::Key { key, pressed, .. } = event else {
            return;
        };
        if !pressed {
            if !self.kept.remove(&key) {
                seat.notify_key(key, false);
            }
            return;
        }
        self.kept.remove(&key);
        if binding(event) {
            self.kept.insert(key);
        } else {
            seat.notify_key(key, true);
        }
    }
}
