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
        Drm,
    }

    #[derive(Debug, Clone)]
    struct Config {
        backend: BackendKind,
        width: i32,
        height: i32,
        server_side_decorations: bool,
        drm_device: String,
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
        SessionActive,
        RequestActivate,
        OutputChange,
        SessionLock,
        WorkspaceActivate,
        TouchDown,
        TouchMotion,
        TouchUp,
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

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
    struct Rect {
        x: i32,
        y: i32,
        width: i32,
        height: i32,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    struct Mode {
        width: i32,
        height: i32,
        refresh_mhz: i32,
        preferred: bool,
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
        fn set_minimized(self: Pin<&mut Server>, window: u64, on: bool);
        fn set_tiled(self: Pin<&mut Server>, window: u64, on: bool);
        fn set_resizing(self: Pin<&mut Server>, window: u64, on: bool);
        fn close(self: Pin<&mut Server>, window: u64);
        fn window_title(self: &Server, window: u64) -> String;
        fn window_app_id(self: &Server, window: u64) -> String;
        fn window_icon_name(self: &Server, window: u64) -> String;
        fn window_size(self: &Server, window: u64) -> Size;
        fn window_min_size(self: &Server, window: u64) -> Size;
        fn window_parent(self: &Server, window: u64) -> u64;

        fn grab_pointer(self: Pin<&mut Server>, on: bool);
        fn bind_key(self: Pin<&mut Server>, mods: u32, keysym: u32) -> u32;
        fn set_keyboard_layout(self: Pin<&mut Server>, layout: &str) -> bool;

        fn output_box(self: &Server, output: u64) -> Rect;
        fn output_usable(self: &Server, output: u64) -> Rect;
        fn output_modes(self: &Server, output: u64) -> Vec<Mode>;
        fn set_output_mode(
            self: Pin<&mut Server>,
            output: u64,
            width: i32,
            height: i32,
            refresh_mhz: i32,
            error: &mut String,
        ) -> bool;
        fn set_output_scale(self: Pin<&mut Server>, output: u64, scale: i32);
        fn set_output_transform(self: Pin<&mut Server>, output: u64, transform: u32);
        fn set_output_position(self: Pin<&mut Server>, output: u64, x: i32, y: i32);

        fn add_workspace(self: Pin<&mut Server>, name: &str) -> u32;
        fn remove_workspace(self: Pin<&mut Server>, workspace: u32);
        fn activate_workspace(self: Pin<&mut Server>, workspace: u32);

        fn is_locked(self: &Server) -> bool;
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

/// A rectangle in global logical coordinates.
pub use ffi::Rect;

impl Rect {
    pub const fn new(x: i32, y: i32, width: i32, height: i32) -> Self {
        Self {
            x,
            y,
            width,
            height,
        }
    }

    pub fn contains(&self, x: f64, y: f64) -> bool {
        x >= self.x as f64
            && y >= self.y as f64
            && x < (self.x + self.width) as f64
            && y < (self.y + self.height) as f64
    }

    /// The rectangle an interactive resize from `self` reaches after the
    /// pointer moved by (`dx`, `dy`): the edges in `edges` (an
    /// [`Event::RequestResize`] bitmask, see [`edge`]) follow the pointer, the
    /// opposite ones stay put, and neither axis shrinks below `min`.
    pub fn resized(&self, edges: u32, dx: i32, dy: i32, min: (i32, i32)) -> Rect {
        let (min_w, min_h) = (min.0.max(1), min.1.max(1));
        let mut r = *self;
        if edges & edge::LEFT != 0 {
            let w = (self.width - dx).max(min_w);
            r.x = self.x + self.width - w;
            r.width = w;
        } else if edges & edge::RIGHT != 0 {
            r.width = (self.width + dx).max(min_w);
        }
        if edges & edge::TOP != 0 {
            let h = (self.height - dy).max(min_h);
            r.y = self.y + self.height - h;
            r.height = h;
        } else if edges & edge::BOTTOM != 0 {
            r.height = (self.height + dy).max(min_h);
        }
        r
    }
}

/// The bits of xdg_toplevel's resize edge, as [`Event::RequestResize`] carries it.
pub mod edge {
    pub const TOP: u32 = 1;
    pub const BOTTOM: u32 = 2;
    pub const LEFT: u32 = 4;
    pub const RIGHT: u32 = 8;
}

/// One display mode a monitor supports.
pub use ffi::Mode;

/// How an output is rotated / flipped (wl_output.transform).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum Transform {
    #[default]
    Normal,
    Rotate90,
    Rotate180,
    Rotate270,
    Flipped,
    Flipped90,
    Flipped180,
    Flipped270,
}

/// A workspace, as pagers (ext-workspace) see it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct WorkspaceId(u32);

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
    /// The first that works of: a window in the parent compositor (when
    /// `WAYLAND_DISPLAY` reaches one); [`Backend::Drm`], but only when a
    /// libseat session can be had, i.e. on a real VT and not over ssh or in
    /// CI; headless.
    #[default]
    Auto,
    /// A window in the parent compositor, or fail.
    Nested,
    /// No display at all; for tests and CI.
    Headless,
    /// Bare metal: KMS for the screens, libinput for input, libseat for
    /// device access and VT switching. Run it from a free VT.
    ///
    /// Without a libseat session (no seatd or logind seat) it still starts
    /// from a logged-in VT, but VT switching is then disabled.
    /// Ctrl+Alt+F1..F12 switch VTs and are never seen by the window manager.
    Drm,
}

#[derive(Debug, Clone)]
pub struct Config {
    pub backend: Backend,
    /// Output size in pixels (the nested window, or the headless output).
    /// Ignored on [`Backend::Drm`], where each monitor keeps its own mode.
    pub width: i32,
    pub height: i32,
    /// Ask clients not to draw their own title bars. You then draw whatever
    /// frame you want with [`Item::Border`] / [`Item::Rect`].
    pub server_side_decorations: bool,
    /// [`Backend::Drm`] only: the card to drive, e.g. `/dev/dri/card1`.
    /// `None` picks the first card with a connected monitor.
    pub drm_device: Option<String>,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            backend: Backend::Auto,
            width: 1280,
            height: 800,
            server_side_decorations: false,
            drm_device: None,
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
    /// Minimize (`on`) or restore: from the window itself, or a taskbar.
    /// Hide it yourself, then confirm with [`Server::set_minimized`].
    RequestMinimize {
        window: WindowId,
        on: bool,
    },
    /// Raise and focus this window: a taskbar click, or an app handing focus
    /// to another through a valid xdg-activation token.
    RequestActivate(WindowId),
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
    /// A finger touched the screen ([`Backend::Drm`] only: libinput
    /// touchscreens). Global logical coordinates, and the window under the
    /// finger. Already forwarded to the client there; this copy is for your
    /// own gestures. `id` is the touch point: unique among the points down at
    /// once, reused after it lifts.
    TouchDown {
        id: i32,
        window: Option<WindowId>,
        x: f64,
        y: f64,
    },
    /// Point `id` moved. `window` is still the one it went down on, wherever
    /// the finger is now — the client keeps getting it, as with a held button.
    TouchMotion {
        id: i32,
        window: Option<WindowId>,
        x: f64,
        y: f64,
    },
    /// Point `id` lifted, or was cancelled (a VT switch, the device went
    /// away); either way it is gone.
    TouchUp {
        id: i32,
        window: Option<WindowId>,
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
    /// The output's box or usable area changed: a mode, scale, transform or
    /// position change, or a panel claiming or releasing an exclusive zone.
    /// Re-read [`Server::output_box`] / [`Server::output_usable`].
    OutputChange(OutputId),
    /// The user closed the nested window. Nothing is torn down; stopping is up to you.
    CloseRequested,
    /// [`Backend::Drm`]: our VT was switched away from (`false`) or back to
    /// (`true`). While inactive there is no input and nothing reaches the
    /// screen; held keys have already been released to clients.
    SessionActive {
        active: bool,
    },
    /// The screen locker (ext-session-lock) locked or unlocked the session.
    /// While locked only the lock screen is drawn and gets input, and no
    /// binding fires; there is nothing to do here but, say, pause things.
    /// A locker that crashes leaves the session locked until another unlocks it.
    SessionLock {
        locked: bool,
    },
    /// A pager asked to switch to this workspace. Do it, then
    /// [`Server::activate_workspace`].
    WorkspaceActivate(WorkspaceId),
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
            K::RequestMinimize => Event::RequestMinimize { window, on: e.on },
            K::RequestActivate => Event::RequestActivate(window),
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
            K::TouchDown => Event::TouchDown {
                id: e.code as i32,
                window: under,
                x: e.x,
                y: e.y,
            },
            K::TouchMotion => Event::TouchMotion {
                id: e.code as i32,
                window: under,
                x: e.x,
                y: e.y,
            },
            K::TouchUp => Event::TouchUp {
                id: e.code as i32,
                window: under,
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
            K::OutputChange => Event::OutputChange(OutputId(e.output)),
            K::CloseRequested => Event::CloseRequested,
            K::SessionActive => Event::SessionActive { active: e.on },
            K::SessionLock => Event::SessionLock { locked: e.on },
            K::WorkspaceActivate => Event::WorkspaceActivate(WorkspaceId(e.code)),
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
                Backend::Drm => ffi::BackendKind::Drm,
            },
            width: config.width,
            height: config.height,
            server_side_decorations: config.server_side_decorations,
            drm_device: config.drm_device.unwrap_or_default(),
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

    /// The smallest size the client says it can be drawn at, `(0, 0)` for none.
    pub fn min_size(&self, window: WindowId) -> (i32, i32) {
        let s = self.raw.window_min_size(window.0);
        (s.width, s.height)
    }

    /// The window this one is a dialog of, if any.
    pub fn parent(&self, window: WindowId) -> Option<WindowId> {
        match self.raw.window_parent(window.0) {
            0 => None,
            id => Some(WindowId(id)),
        }
    }

    /// The icon name the client set (xdg-toplevel-icon), empty when none.
    pub fn icon_name(&self, window: WindowId) -> String {
        self.raw.window_icon_name(window.0)
    }

    /// Tell the window (and taskbars) that you have hidden it, or shown it again.
    pub fn set_minimized(&mut self, window: WindowId, on: bool) {
        self.pin().set_minimized(window.0, on);
    }

    /// Tiled windows drop their shadows and rounded corners.
    pub fn set_tiled(&mut self, window: WindowId, on: bool) {
        self.pin().set_tiled(window.0, on);
    }

    /// Bracket an interactive resize: clients redraw cheaper while it is on.
    pub fn set_resizing(&mut self, window: WindowId, on: bool) {
        self.pin().set_resizing(window.0, on);
    }

    /// While on, the pointer is yours: clients see no motion or scrolling
    /// (buttons still reach them, so the press that started a drag ends
    /// cleanly). Turn it on for an interactive move or resize, off at the end.
    pub fn grab_pointer(&mut self, on: bool) {
        self.pin().grab_pointer(on);
    }

    /// Switch the keyboard layout, e.g. `"de"` or `"us,ru"`. Bare metal only:
    /// nested, keys arrive in the host's layout. False if xkb rejects it.
    pub fn set_keyboard_layout(&mut self, layout: &str) -> bool {
        self.pin().set_keyboard_layout(layout)
    }

    /// Where the output sits in the global layout, in logical pixels.
    pub fn output_box(&self, output: OutputId) -> Rect {
        self.raw.output_box(output.0)
    }

    /// [`Server::output_box`] minus what panels reserved (layer-shell
    /// exclusive zones): where windows should go.
    pub fn output_usable(&self, output: OutputId) -> Rect {
        self.raw.output_usable(output.0)
    }

    /// The modes the monitor supports; empty for nested and headless outputs.
    pub fn output_modes(&self, output: OutputId) -> Vec<Mode> {
        self.raw.output_modes(output.0)
    }

    /// Change the monitor's mode. `refresh_mhz` 0 picks the best rate for the
    /// size. Followed by [`Event::OutputChange`].
    pub fn set_output_mode(&mut self, output: OutputId, mode: Mode) -> Result<(), Error> {
        let mut error = String::new();
        let raw = self.pin();
        if raw.set_output_mode(
            output.0,
            mode.width,
            mode.height,
            mode.refresh_mhz,
            &mut error,
        ) {
            Ok(())
        } else {
            Err(Error(error))
        }
    }

    /// Integer scale (HiDPI). Followed by [`Event::OutputChange`].
    pub fn set_output_scale(&mut self, output: OutputId, scale: i32) {
        self.pin().set_output_scale(output.0, scale.max(1));
    }

    /// Rotate or flip the output. Followed by [`Event::OutputChange`].
    pub fn set_output_transform(&mut self, output: OutputId, transform: Transform) {
        self.pin().set_output_transform(output.0, transform as u32);
    }

    /// Move the output in the global layout. Followed by [`Event::OutputChange`].
    pub fn set_output_position(&mut self, output: OutputId, x: i32, y: i32) {
        self.pin().set_output_position(output.0, x, y);
    }

    /// Publish a workspace to pagers. What a workspace *is* stays yours.
    pub fn add_workspace(&mut self, name: &str) -> WorkspaceId {
        WorkspaceId(self.pin().add_workspace(name))
    }

    pub fn remove_workspace(&mut self, workspace: WorkspaceId) {
        self.pin().remove_workspace(workspace.0);
    }

    /// Mark it the active one (and every other inactive).
    pub fn activate_workspace(&mut self, workspace: WorkspaceId) {
        self.pin().activate_workspace(workspace.0);
    }

    /// Whether the lock screen is up. See [`Event::SessionLock`].
    pub fn is_locked(&self) -> bool {
        self.raw.is_locked()
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
