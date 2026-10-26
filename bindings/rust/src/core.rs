//! Core library primitives: Display, EventLoop, errors, and input routers.

use std::collections::HashSet;
use std::ffi::c_void;
use std::fmt;
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, RawFd};
use std::time::Duration;

use crate::ffi;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(pub(crate) String);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

/// The Wayland display server context. Owns `wl_display` and the main event loop.
pub struct Display {
    pub(crate) raw: cxx::UniquePtr<ffi::DisplayShim>,
}

impl Display {
    pub fn new() -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::display_new(&mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    /// Auto-bind a socket (`wayland-0`, `wayland-1`, ...) and return its name.
    pub fn add_socket_auto(&self) -> Result<String, Error> {
        let mut err = String::new();
        let name = self.raw.add_socket_auto(&mut err);
        if name.is_empty() {
            Err(Error(err))
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
            Err(Error(err))
        }
    }

    /// Initialize built-in `wl_shm` support.
    pub fn init_shm(&self) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.init_shm(&mut err) {
            Ok(())
        } else {
            Err(Error(err))
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

    /// Wait up to `timeout` for events on the event loop and dispatch them.
    pub fn dispatch(&self, timeout: Option<Duration>) -> Result<i32, Error> {
        let ms = timeout.map_or(-1, |t| t.as_millis().min(i32::MAX as u128) as i32);
        let ret = self.raw.dispatch(ms);
        if ret < 0 {
            Err(Error(format!("event loop dispatch error: {ret}")))
        } else {
            Ok(ret)
        }
    }

    /// Flush pending requests/events to all connected clients.
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

    /// Get a non-owning view of the event loop.
    pub fn event_loop(&self) -> EventLoop {
        EventLoop {
            fd: self.raw.fd(),
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

/// A non-owning handle to the event loop.
#[derive(Clone, Copy)]
pub struct EventLoop {
    fd: RawFd,
}

impl EventLoop {
    pub fn fd(&self) -> RawFd {
        self.fd
    }
}

impl AsRawFd for EventLoop {
    fn as_raw_fd(&self) -> RawFd {
        self.fd
    }
}

impl AsFd for EventLoop {
    fn as_fd(&self) -> BorrowedFd<'_> {
        unsafe { BorrowedFd::borrow_raw(self.fd) }
    }
}

/// Tracks swallowed keypresses to ensure matching releases are also swallowed.
#[derive(Default)]
pub struct KeyRouter {
    consumed: HashSet<u32>,
}

impl KeyRouter {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn consume(&mut self, keycode: u32) {
        self.consumed.insert(keycode);
    }

    pub fn unconsume(&mut self, keycode: u32) -> bool {
        self.consumed.remove(&keycode)
    }

    pub fn is_consumed(&self, keycode: u32) -> bool {
        self.consumed.contains(&keycode)
    }
}
