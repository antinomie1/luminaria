//! Rust bindings for luminaria, a Wayland compositor library.
//!
//! The C++ side (`luminaria_rs.cpp`) does everything every compositor does
//! the same way: globals, renderer, frame pacing, frame callbacks, popups,
//! the cursor, pointer focus. What is left is window management, and that is
//! this API:
//!
//! ```no_run
//! use luminaria::{Config, Event, Item, Server};
//!
//! let mut server = Server::new(Config::default()).unwrap();
//! println!("WAYLAND_DISPLAY={}", server.socket_name());
//! let mut windows = Vec::new();
//! server.run(|server, event| {
//!     match event {
//!         Event::WindowMap(w) => {
//!             windows.push(w);
//!             server.focus(Some(w));
//!         }
//!         Event::WindowDestroy(w) => windows.retain(|&x| x != w),
//!         Event::CloseRequested => return std::ops::ControlFlow::Break(()),
//!         _ => return std::ops::ControlFlow::Continue(()),
//!     }
//!     let scene: Vec<Item> = windows
//!         .iter()
//!         .enumerate()
//!         .map(|(i, &id)| Item::Window { id, x: 40 + 30 * i as i32, y: 40 + 30 * i as i32 })
//!         .collect();
//!     server.set_scene(&scene);
//!     std::ops::ControlFlow::Continue(())
//! })
//! .unwrap();
//! ```
//!
//! # Threading and re-entrancy
//!
//! C++ never calls into Rust. Wayland callbacks only queue [`Event`]s, which
//! you take with [`Server::next_event`] after [`Server::dispatch`] returns, so
//! every method here is called while no C++ callback is on the stack. That is
//! why `&mut Server` can simply be handed to your event handler.
//!
//! [`Server`] is neither `Send` nor `Sync`: libwayland is single-threaded.

use std::fmt;
use std::ops::{BitOr, BitOrAssign, ControlFlow};
use std::os::fd::{AsRawFd, RawFd};
use std::time::Duration;

#[cxx::bridge(namespace = "lum")]
mod ffi {
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum BackendKind {
        Auto,
        Nested,
        Headless,
    }

    #[derive(Debug, Clone)]
    struct Config {
        backend: BackendKind,
        width: i32,
        height: i32,
        server_side_decorations: bool,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum EventKind {
        WindowNew,
        WindowMap,
        WindowUnmap,
        WindowDestroy,
        WindowTitle,
        RequestMaximize,
        RequestFullscreen,
        RequestMinimize,
        RequestMove,
        RequestResize,
        PointerMotion,
        PointerButton,
        Binding,
        OutputNew,
        OutputDestroy,
        CloseRequested,
    }

    /// Flat on purpose: cxx cannot carry a Rust enum with payloads, so every
    /// kind uses the fields it needs and `Event::from_raw` gives it a shape.
    #[derive(Debug, Clone, Copy)]
    struct Event {
        kind: EventKind,
        window: u64,
        output: u64,
        on: bool,
        code: u32,
        x: f64,
        y: f64,
        width: i32,
        height: i32,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum ItemKind {
        Window,
        Rect,
        Border,
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct Color {
        r: f32,
        g: f32,
        b: f32,
        a: f32,
    }

    #[derive(Debug, Clone, Copy)]
    struct Item {
        kind: ItemKind,
        window: u64,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        thickness: i32,
        color: Color,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct Size {
        width: i32,
        height: i32,
    }

    unsafe extern "C++" {
        include!("luminaria/luminaria_rs.h");

        type Server;

        fn server_new(config: &Config, error: &mut String) -> UniquePtr<Server>;
        fn keysym_from_name(name: &str) -> u32;

        fn socket_name(self: &Server) -> String;
        fn fd(self: &Server) -> i32;
        fn dispatch(self: Pin<&mut Server>, timeout_ms: i32) -> bool;
        fn next_event(self: Pin<&mut Server>, out: &mut Event) -> bool;

        fn set_scene(self: Pin<&mut Server>, items: &[Item]);
        fn set_background(self: Pin<&mut Server>, r: f32, g: f32, b: f32, a: f32);

        fn focus(self: Pin<&mut Server>, window: u64);
        fn configure(self: Pin<&mut Server>, window: u64, width: i32, height: i32);
        fn set_maximized(self: Pin<&mut Server>, window: u64, on: bool);
        fn set_fullscreen(self: Pin<&mut Server>, window: u64, on: bool);
        fn close(self: Pin<&mut Server>, window: u64);
        fn window_title(self: &Server, window: u64) -> String;
        fn window_app_id(self: &Server, window: u64) -> String;
        fn window_size(self: &Server, window: u64) -> Size;

        fn bind_key(self: Pin<&mut Server>, mods: u32, keysym: u32) -> u32;
    }
}

/// An RGBA colour, each channel in `0.0..=1.0`, not premultiplied.
pub use ffi::Color;

impl Color {
    pub const fn rgb(r: f32, g: f32, b: f32) -> Self {
        Self { r, g, b, a: 1.0 }
    }
    pub const fn rgba(r: f32, g: f32, b: f32, a: f32) -> Self {
        Self { r, g, b, a }
    }
}

/// A toplevel window. Never reused, so a stale id simply stops resolving:
/// every call taking one is a no-op once the window is gone.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct WindowId(u64);

/// A display output. Never reused.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct OutputId(u64);

/// What [`Server::bind`] / [`Server::bind_key`] returned.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct BindingId(u32);

/// Modifier set for a key binding.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub struct Mods(u32);

impl Mods {
    // xkb's core modifier bits; the same for every keymap.
    pub const NONE: Mods = Mods(0);
    pub const SHIFT: Mods = Mods(1 << 0);
    pub const CTRL: Mods = Mods(1 << 2);
    pub const ALT: Mods = Mods(1 << 3);
    pub const LOGO: Mods = Mods(1 << 6);
}

impl BitOr for Mods {
    type Output = Mods;
    fn bitor(self, rhs: Mods) -> Mods {
        Mods(self.0 | rhs.0)
    }
}

impl BitOrAssign for Mods {
    fn bitor_assign(&mut self, rhs: Mods) {
        self.0 |= rhs.0;
    }
}

/// Where the compositor shows up.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Backend {
    /// A window in the parent compositor if `WAYLAND_DISPLAY` reaches one,
    /// otherwise headless.
    #[default]
    Auto,
    /// A window in the parent compositor, or fail.
    Nested,
    /// No display at all; for tests and CI.
    Headless,
}

#[derive(Debug, Clone)]
pub struct Config {
    pub backend: Backend,
    /// Output size in pixels (the nested window, or the headless output).
    pub width: i32,
    pub height: i32,
    /// Ask clients not to draw their own title bars. You then draw whatever
    /// frame you want with [`Item::Border`] / [`Item::Rect`].
    pub server_side_decorations: bool,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            backend: Backend::Auto,
            width: 1280,
            height: 800,
            server_side_decorations: false,
        }
    }
}

/// Something happened. Taken with [`Server::next_event`], or handed to the
/// closure given to [`Server::run`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Event {
    /// A client created a toplevel. Not drawable until [`Event::WindowMap`].
    WindowNew(WindowId),
    /// The window has content: put it in the scene.
    WindowMap(WindowId),
    /// Hidden by the client. Keyboard focus, if it had it, is already gone.
    WindowUnmap(WindowId),
    /// Gone for good. The id no longer resolves.
    WindowDestroy(WindowId),
    /// Title or app id changed; read them with [`Server::title`] / [`Server::app_id`].
    WindowTitle(WindowId),
    /// Answer with [`Server::set_maximized`] (or ignore it).
    RequestMaximize {
        window: WindowId,
        on: bool,
    },
    /// Answer with [`Server::set_fullscreen`] (or ignore it).
    RequestFullscreen {
        window: WindowId,
        on: bool,
    },
    RequestMinimize(WindowId),
    /// The client wants an interactive move (it was dragged by its title bar).
    RequestMove(WindowId),
    /// The client wants an interactive resize; `edges` is xdg_toplevel's
    /// resize_edge bitmask.
    RequestResize {
        window: WindowId,
        edges: u32,
    },
    /// Global logical coordinates, and the window under the pointer.
    PointerMotion {
        window: Option<WindowId>,
        x: f64,
        y: f64,
    },
    /// Already forwarded to the client under the pointer; this is a copy for
    /// you, e.g. for click-to-focus. `button` is an evdev code (`BTN_LEFT` = 272).
    PointerButton {
        window: Option<WindowId>,
        button: u32,
        pressed: bool,
        x: f64,
        y: f64,
    },
    /// A key binding fired. The key was not forwarded to any client.
    Binding(BindingId),
    OutputNew {
        output: OutputId,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
    },
    OutputDestroy(OutputId),
    /// The user closed the nested window. Nothing is torn down; stopping is up to you.
    CloseRequested,
}

impl Event {
    fn from_raw(e: &ffi::Event) -> Option<Event> {
        use ffi::EventKind as K;
        let window = WindowId(e.window);
        let under = (e.window != 0).then_some(window);
        Some(match e.kind {
            K::WindowNew => Event::WindowNew(window),
            K::WindowMap => Event::WindowMap(window),
            K::WindowUnmap => Event::WindowUnmap(window),
            K::WindowDestroy => Event::WindowDestroy(window),
            K::WindowTitle => Event::WindowTitle(window),
            K::RequestMaximize => Event::RequestMaximize { window, on: e.on },
            K::RequestFullscreen => Event::RequestFullscreen { window, on: e.on },
            K::RequestMinimize => Event::RequestMinimize(window),
            K::RequestMove => Event::RequestMove(window),
            K::RequestResize => Event::RequestResize {
                window,
                edges: e.code,
            },
            K::PointerMotion => Event::PointerMotion {
                window: under,
                x: e.x,
                y: e.y,
            },
            K::PointerButton => Event::PointerButton {
                window: under,
                button: e.code,
                pressed: e.on,
                x: e.x,
                y: e.y,
            },
            K::Binding => Event::Binding(BindingId(e.code)),
            K::OutputNew => Event::OutputNew {
                output: OutputId(e.output),
                x: e.x as i32,
                y: e.y as i32,
                width: e.width,
                height: e.height,
            },
            K::OutputDestroy => Event::OutputDestroy(OutputId(e.output)),
            K::CloseRequested => Event::CloseRequested,
            _ => return None,
        })
    }
}

/// One entry in the scene. The list is back to front, in global logical
/// coordinates, and it is both what is drawn and what the pointer hits.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Item {
    /// A window with its top-left at (`x`, `y`). That is the corner of the
    /// window's *content*, excluding any client-drawn shadow, and the window
    /// is clipped to it. Its popups follow it automatically.
    Window { id: WindowId, x: i32, y: i32 },
    /// A solid rectangle. Not hit-tested.
    Rect {
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        color: Color,
    },
    /// A frame `thickness` wide just inside the box. Not hit-tested.
    Border {
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        thickness: i32,
        color: Color,
    },
}

impl Item {
    fn to_raw(self) -> ffi::Item {
        let mut raw = ffi::Item {
            kind: ffi::ItemKind::Window,
            window: 0,
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            thickness: 0,
            color: Color::rgba(0.0, 0.0, 0.0, 0.0),
        };
        match self {
            Item::Window { id, x, y } => {
                raw.window = id.0;
                (raw.x, raw.y) = (x, y);
            }
            Item::Rect {
                x,
                y,
                width,
                height,
                color,
            } => {
                raw.kind = ffi::ItemKind::Rect;
                (raw.x, raw.y, raw.width, raw.height, raw.color) = (x, y, width, height, color);
            }
            Item::Border {
                x,
                y,
                width,
                height,
                thickness,
                color,
            } => {
                raw.kind = ffi::ItemKind::Border;
                (raw.x, raw.y, raw.width, raw.height) = (x, y, width, height);
                (raw.thickness, raw.color) = (thickness, color);
            }
        }
        raw
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(String);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

/// The compositor. Owns the Wayland socket for as long as it lives.
pub struct Server {
    raw: cxx::UniquePtr<ffi::Server>,
}

impl Server {
    pub fn new(config: Config) -> Result<Server, Error> {
        let raw_config = ffi::Config {
            backend: match config.backend {
                Backend::Auto => ffi::BackendKind::Auto,
                Backend::Nested => ffi::BackendKind::Nested,
                Backend::Headless => ffi::BackendKind::Headless,
            },
            width: config.width,
            height: config.height,
            server_side_decorations: config.server_side_decorations,
        };
        let mut error = String::new();
        let raw = ffi::server_new(&raw_config, &mut error);
        if raw.is_null() {
            return Err(Error(error));
        }
        Ok(Server { raw })
    }

    fn pin(&mut self) -> std::pin::Pin<&mut ffi::Server> {
        self.raw.pin_mut()
    }

    /// The socket clients connect to: set `WAYLAND_DISPLAY` to this.
    pub fn socket_name(&self) -> String {
        self.raw.socket_name()
    }

    /// Wait up to `timeout` (forever with `None`) for something to happen and
    /// handle it. Afterwards, drain [`Server::next_event`].
    pub fn dispatch(&mut self, timeout: Option<Duration>) -> Result<(), Error> {
        let ms = timeout.map_or(-1, |t| t.as_millis().min(i32::MAX as u128) as i32);
        if self.pin().dispatch(ms) {
            Ok(())
        } else {
            Err(Error(format!(
                "event loop: {}",
                std::io::Error::last_os_error()
            )))
        }
    }

    pub fn next_event(&mut self) -> Option<Event> {
        let mut raw = ffi::Event {
            kind: ffi::EventKind::CloseRequested,
            window: 0,
            output: 0,
            on: false,
            code: 0,
            x: 0.0,
            y: 0.0,
            width: 0,
            height: 0,
        };
        while self.pin().next_event(&mut raw) {
            if let Some(event) = Event::from_raw(&raw) {
                return Some(event);
            }
        }
        None
    }

    /// `dispatch` + `next_event` in a loop, until `handler` breaks.
    pub fn run<F>(&mut self, mut handler: F) -> Result<(), Error>
    where
        F: FnMut(&mut Server, Event) -> ControlFlow<()>,
    {
        loop {
            self.dispatch(None)?;
            while let Some(event) = self.next_event() {
                if handler(self, event).is_break() {
                    return Ok(());
                }
            }
        }
    }

    /// Replace the whole scene. Call it whenever your layout changes; the
    /// compositor redraws only what actually moved.
    pub fn set_scene(&mut self, items: &[Item]) {
        let raw: Vec<ffi::Item> = items.iter().map(|i| i.to_raw()).collect();
        self.pin().set_scene(&raw);
    }

    pub fn set_background(&mut self, color: Color) {
        self.pin()
            .set_background(color.r, color.g, color.b, color.a);
    }

    /// Keyboard focus, and the window's "activated" look. `None` unfocuses.
    pub fn focus(&mut self, window: Option<WindowId>) {
        self.pin().focus(window.map_or(0, |w| w.0));
    }

    /// Ask the window to take this size. 0 lets the client choose.
    pub fn configure(&mut self, window: WindowId, width: i32, height: i32) {
        self.pin().configure(window.0, width, height);
    }

    pub fn set_maximized(&mut self, window: WindowId, on: bool) {
        self.pin().set_maximized(window.0, on);
    }

    pub fn set_fullscreen(&mut self, window: WindowId, on: bool) {
        self.pin().set_fullscreen(window.0, on);
    }

    /// Politely: the client may ask the user first, or ignore it.
    pub fn close(&mut self, window: WindowId) {
        self.pin().close(window.0);
    }

    pub fn title(&self, window: WindowId) -> String {
        self.raw.window_title(window.0)
    }

    pub fn app_id(&self, window: WindowId) -> String {
        self.raw.window_app_id(window.0)
    }

    /// The window's content size, `(0, 0)` before it has any.
    pub fn size(&self, window: WindowId) -> (i32, i32) {
        let s = self.raw.window_size(window.0);
        (s.width, s.height)
    }

    /// Swallow `keysym` pressed with exactly `mods` and report it as
    /// [`Event::Binding`] instead of sending it to the focused client.
    pub fn bind_key(&mut self, mods: Mods, keysym: u32) -> BindingId {
        BindingId(self.pin().bind_key(mods.0, keysym))
    }

    /// [`Server::bind_key`] from a spec like `"Logo+Shift+q"` or `"Ctrl+Alt+Delete"`.
    /// Modifier names: `Shift`, `Ctrl`, `Alt`, `Logo` (also `Super`).
    pub fn bind(&mut self, spec: &str) -> Result<BindingId, Error> {
        let mut mods = Mods::NONE;
        let mut parts: Vec<&str> = spec.split('+').map(str::trim).collect();
        let key = parts.pop().filter(|k| !k.is_empty());
        let key = key.ok_or_else(|| Error(format!("binding {spec:?}: no key")))?;
        for part in parts {
            mods |= match part.to_ascii_lowercase().as_str() {
                "shift" => Mods::SHIFT,
                "ctrl" | "control" => Mods::CTRL,
                "alt" => Mods::ALT,
                "logo" | "super" | "mod4" => Mods::LOGO,
                _ => {
                    return Err(Error(format!(
                        "binding {spec:?}: unknown modifier {part:?}"
                    )));
                }
            };
        }
        match keysym(key) {
            0 => Err(Error(format!("binding {spec:?}: unknown key {key:?}"))),
            sym => Ok(self.bind_key(mods, sym)),
        }
    }
}

impl AsRawFd for Server {
    /// The event loop's fd, for embedding in another loop: when it is
    /// readable, call `dispatch(Some(Duration::ZERO))`.
    fn as_raw_fd(&self) -> RawFd {
        self.raw.fd()
    }
}

/// The xkb keysym called `name` ("Return", "q", "F1"), or 0. Case is ignored
/// when the exact name does not exist.
pub fn keysym(name: &str) -> u32 {
    ffi::keysym_from_name(name)
}
