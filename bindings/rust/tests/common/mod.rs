//! The client half of the protocol tests, shared by every test that drives
//! real protocol traffic: a libwayland client (`wayland-client` over
//! `wayland-backend`'s system backend) on one end of a socketpair whose other
//! end is handed to luminaria's display with `wl_client_create`, the way the
//! C++ tests connect. No socket, no XDG_RUNTIME_DIR.
//!
//! The client runs a script on its own thread; the test thread dispatches the
//! display and steps through it with [`Peer`]: the client says which point it
//! reached ([`ClientSide::step`]) and waits for the test to inspect and act on
//! the server side, then to let it go on ([`Peer::go`]).
#![allow(dead_code, unused_macros)]

use std::ffi::c_void;
use std::fs::{File, OpenOptions};
use std::os::fd::{AsFd, IntoRawFd};
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use luminaria::Event;
use luminaria::core::Display;
use wayland_client::protocol::{wl_buffer, wl_shm, wl_shm_pool};
use wayland_client::{Connection, Dispatch, QueueHandle};

unsafe extern "C" {
    fn wl_client_create(display: *mut c_void, fd: i32) -> *mut c_void;
}

const TIMEOUT: Duration = Duration::from_secs(10);

thread_local! {
    /// Every event the display handed back and no test has taken yet.
    static INBOX: std::cell::RefCell<Vec<Event>> = const { std::cell::RefCell::new(Vec::new()) };
}

/// One dispatch of the display, flushing what it produced to the clients. Its
/// events go to the inbox, for [`take`].
pub fn dispatch(display: &Display) {
    let events = display.dispatch(Some(Duration::from_millis(5))).expect("dispatch");
    display.flush_clients();
    INBOX.with_borrow_mut(|inbox| inbox.extend(events));
}

/// What the compositor's own calls queued since the last dispatch (a signal
/// fired outside one), into the inbox without waiting.
pub fn collect(display: &Display) {
    let events = display.events();
    INBOX.with_borrow_mut(|inbox| inbox.extend(events));
}

/// Every event of one kind in the inbox, oldest first, taken out of it; the
/// others stay for whoever asks for them.
pub fn take<T: TryFrom<Event, Error = Event>>() -> Vec<T> {
    INBOX.with_borrow_mut(|inbox| {
        let mut taken = Vec::new();
        let mut kept = Vec::new();
        for event in inbox.drain(..) {
            match T::try_from(event) {
                Ok(t) => taken.push(t),
                Err(other) => kept.push(other),
            }
        }
        *inbox = kept;
        taken
    })
}

/// Forget everything in the inbox.
pub fn clear_inbox() {
    INBOX.with_borrow_mut(Vec::clear);
}

/// Dispatch until `done`, for at most ten seconds.
pub fn pump(display: &Display, what: &str, mut done: impl FnMut() -> bool) {
    let deadline = Instant::now() + TIMEOUT;
    while !done() {
        assert!(Instant::now() < deadline, "timed out waiting for {what}");
        dispatch(display);
    }
}

/// Drain an event queue into a Vec.
pub fn drain<T>(mut next: impl FnMut() -> Option<T>) -> Vec<T> {
    std::iter::from_fn(|| next()).collect()
}

/// The test's handle on the client thread.
pub struct Peer {
    reached: Receiver<&'static str>,
    go: Sender<()>,
    thread: Option<JoinHandle<()>>,
}

/// What the client script steps with.
pub struct ClientSide {
    reached: Sender<&'static str>,
    go: Receiver<()>,
}

impl ClientSide {
    /// Tell the test `what` was reached, and wait until it says go on.
    pub fn step(&self, what: &'static str) {
        self.reached.send(what).expect("test thread gone");
        self.go.recv().expect("test thread gone");
    }
}

impl Peer {
    /// Connect a client to `display` and run `script` on it in a thread.
    pub fn spawn(display: &Display, script: impl FnOnce(Connection, ClientSide) + Send + 'static) -> Self {
        let (server, client) = UnixStream::pair().expect("socketpair");
        // SAFETY: the display is live, and libwayland takes the fd over.
        let c = unsafe { wl_client_create(display.as_ptr(), server.into_raw_fd()) };
        assert!(!c.is_null(), "wl_client_create");
        let (reached_tx, reached) = mpsc::channel();
        let (go, go_rx) = mpsc::channel();
        let thread = std::thread::spawn(move || {
            let conn = Connection::from_socket(client).expect("wayland connection");
            script(conn, ClientSide { reached: reached_tx, go: go_rx });
        });
        Self { reached, go, thread: Some(thread) }
    }

    /// Dispatch until the client reaches `what`. A client that panicked
    /// fails the test with its own message.
    pub fn wait(&mut self, display: &Display, what: &str) {
        let deadline = Instant::now() + TIMEOUT;
        loop {
            match self.reached.try_recv() {
                Ok(got) => {
                    assert_eq!(got, what, "the client reached another step");
                    return;
                }
                Err(TryRecvError::Disconnected) => {
                    self.join_thread();
                    panic!("client finished before reaching {what}");
                }
                Err(TryRecvError::Empty) => {}
            }
            assert!(Instant::now() < deadline, "timed out waiting for the client to reach {what}");
            dispatch(display);
        }
    }

    /// Let the client go on from its step.
    pub fn go(&self) {
        let _ = self.go.send(());
    }

    /// Dispatch until the client script returns (it disconnects as it does),
    /// then a few more times so the server sees the hang-up.
    pub fn finish(mut self, display: &Display) {
        self.go();
        let deadline = Instant::now() + TIMEOUT;
        while !self.thread.as_ref().is_some_and(|t| t.is_finished()) {
            assert!(Instant::now() < deadline, "timed out waiting for the client to finish");
            dispatch(display);
        }
        self.join_thread();
        for _ in 0..4 {
            dispatch(display);
        }
    }

    /// Let the client go on and wait for it, without dispatching: for a test
    /// that has already dropped the display under it.
    pub fn join(mut self) {
        self.go();
        self.join_thread();
    }

    fn join_thread(&mut self) {
        if let Some(t) = self.thread.take() {
            if let Err(panic) = t.join() {
                std::panic::resume_unwind(panic);
            }
        }
    }
}

impl Drop for Peer {
    fn drop(&mut self) {
        // A test failing mid-script leaves the client blocked on its step.
        let _ = self.go.send(());
    }
}

/// A file to back a wl_shm pool: unlinked at once, so only the fds remain.
fn shm_file(size: u64) -> File {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let path = std::env::temp_dir().join(format!(
        "luminaria-test-shm-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    let file = OpenOptions::new()
        .read(true)
        .write(true)
        .create_new(true)
        .open(&path)
        .expect("shm file");
    std::fs::remove_file(&path).expect("unlink shm file");
    file.set_len(size).expect("size shm file");
    file
}

/// An ARGB8888 buffer of `width`x`height`, all zero (transparent black).
pub fn shm_buffer<D>(shm: &wl_shm::WlShm, qh: &QueueHandle<D>, width: i32, height: i32) -> wl_buffer::WlBuffer
where
    D: Dispatch<wl_shm_pool::WlShmPool, ()> + Dispatch<wl_buffer::WlBuffer, ()> + 'static,
{
    let stride = width * 4;
    let file = shm_file((stride * height) as u64);
    let pool = shm.create_pool(file.as_fd(), stride * height, qh, ());
    let buffer = pool.create_buffer(0, width, height, stride, wl_shm::Format::Argb8888, qh, ());
    pool.destroy();
    buffer
}

/// Implement `Dispatch<I, ()>` for interfaces whose events a test ignores.
macro_rules! ignore_events {
    ($state:ty: $($iface:ty),* $(,)?) => {
        $(
            impl wayland_client::Dispatch<$iface, ()> for $state {
                fn event(
                    _: &mut Self,
                    _: &$iface,
                    _: <$iface as wayland_client::Proxy>::Event,
                    _: &(),
                    _: &wayland_client::Connection,
                    _: &wayland_client::QueueHandle<Self>,
                ) {
                }
            }
        )*
    };
}

/// The registry, for `registry_queue_init`.
macro_rules! registry_state {
    ($state:ty) => {
        impl wayland_client::Dispatch<wayland_client::protocol::wl_registry::WlRegistry, wayland_client::globals::GlobalListContents>
            for $state
        {
            fn event(
                _: &mut Self,
                _: &wayland_client::protocol::wl_registry::WlRegistry,
                _: wayland_client::protocol::wl_registry::Event,
                _: &wayland_client::globals::GlobalListContents,
                _: &wayland_client::Connection,
                _: &wayland_client::QueueHandle<Self>,
            ) {
            }
        }
    };
}

/// Answer xdg_wm_base pings and ack every xdg_surface configure: enough to
/// give a test client a surface with a role the server announces (and so
/// names) through `XdgShell::next_event`.
macro_rules! xdg_basics {
    ($state:ty) => {
        impl wayland_client::Dispatch<wayland_protocols::xdg::shell::client::xdg_wm_base::XdgWmBase, ()> for $state {
            fn event(
                _: &mut Self,
                wm: &wayland_protocols::xdg::shell::client::xdg_wm_base::XdgWmBase,
                event: wayland_protocols::xdg::shell::client::xdg_wm_base::Event,
                _: &(),
                _: &wayland_client::Connection,
                _: &wayland_client::QueueHandle<Self>,
            ) {
                if let wayland_protocols::xdg::shell::client::xdg_wm_base::Event::Ping { serial } = event {
                    wm.pong(serial);
                }
            }
        }

        impl wayland_client::Dispatch<wayland_protocols::xdg::shell::client::xdg_surface::XdgSurface, ()> for $state {
            fn event(
                _: &mut Self,
                xs: &wayland_protocols::xdg::shell::client::xdg_surface::XdgSurface,
                event: wayland_protocols::xdg::shell::client::xdg_surface::Event,
                _: &(),
                _: &wayland_client::Connection,
                _: &wayland_client::QueueHandle<Self>,
            ) {
                if let wayland_protocols::xdg::shell::client::xdg_surface::Event::Configure { serial } = event {
                    xs.ack_configure(serial);
                }
            }
        }

        ignore_events!($state: wayland_protocols::xdg::shell::client::xdg_toplevel::XdgToplevel);
    };
}
