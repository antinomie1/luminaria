//! High-level opinionated compositor facade (`Server`).

use std::ops::{BitOr, BitOrAssign, ControlFlow};
use std::os::fd::{AsRawFd, RawFd};
use std::time::Duration;

use crate::core::Error;
use crate::ffi;
use crate::util::{Color, Mode, Rect, Transform};

/// The bits of xdg_toplevel's resize edge, as [`Event::RequestResize`] carries it.
pub mod edge {
    pub const TOP: u32 = 1;
    pub const BOTTOM: u32 = 2;
    pub const LEFT: u32 = 4;
    pub const RIGHT: u32 = 8;
}

/// A workspace, as pagers (ext-workspace) see it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct WorkspaceId(pub u32);

/// A toplevel window. Never reused, so a stale id simply stops resolving:
/// every call taking one is a no-op once the window is gone.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct WindowId(pub u64);

/// A display output. Never reused.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct OutputId(pub u64);

/// What [`Server::bind`] / [`Server::bind_key`] returned.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct BindingId(pub u32);

/// Modifier set for a key binding.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub struct Mods(pub u32);

impl Mods {
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
    #[default]
    Auto,
    Nested,
    Headless,
    Drm,
}

#[derive(Debug, Clone)]
pub struct Config {
    pub backend: Backend,
    pub width: i32,
    pub height: i32,
    pub server_side_decorations: bool,
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
    WindowNew(WindowId),
    WindowMap(WindowId),
    WindowUnmap(WindowId),
    WindowDestroy(WindowId),
    WindowTitle(WindowId),
    RequestMaximize {
        window: WindowId,
        on: bool,
    },
    RequestFullscreen {
        window: WindowId,
        on: bool,
    },
    RequestMinimize {
        window: WindowId,
        on: bool,
    },
    RequestActivate(WindowId),
    RequestMove(WindowId),
    RequestResize {
        window: WindowId,
        edges: u32,
    },
    PointerMotion {
        window: Option<WindowId>,
        x: f64,
        y: f64,
    },
    PointerButton {
        window: Option<WindowId>,
        button: u32,
        pressed: bool,
        x: f64,
        y: f64,
    },
    TouchDown {
        id: i32,
        window: Option<WindowId>,
        x: f64,
        y: f64,
    },
    TouchMotion {
        id: i32,
        window: Option<WindowId>,
        x: f64,
        y: f64,
    },
    TouchUp {
        id: i32,
        window: Option<WindowId>,
    },
    Binding(BindingId),
    OutputNew {
        output: OutputId,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
    },
    OutputDestroy(OutputId),
    OutputChange(OutputId),
    CloseRequested,
    SessionActive {
        active: bool,
    },
    SessionLock {
        locked: bool,
    },
    WorkspaceActivate(WorkspaceId),
}

impl Event {
    pub(crate) fn from_raw(e: &ffi::Event) -> Option<Event> {
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

/// One entry in the scene.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Item {
    Window { id: WindowId, x: i32, y: i32 },
    Rect {
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        color: Color,
    },
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
    pub(crate) fn to_raw(self) -> ffi::Item {
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

    pub fn socket_name(&self) -> String {
        self.raw.socket_name()
    }

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

    pub fn set_scene(&mut self, items: &[Item]) {
        let raw: Vec<ffi::Item> = items.iter().map(|i| i.to_raw()).collect();
        self.pin().set_scene(&raw);
    }

    pub fn set_background(&mut self, color: Color) {
        self.pin()
            .set_background(color.r, color.g, color.b, color.a);
    }

    pub fn focus(&mut self, window: Option<WindowId>) {
        self.pin().focus(window.map_or(0, |w| w.0));
    }

    pub fn configure(&mut self, window: WindowId, width: i32, height: i32) {
        self.pin().configure(window.0, width, height);
    }

    pub fn set_maximized(&mut self, window: WindowId, on: bool) {
        self.pin().set_maximized(window.0, on);
    }

    pub fn set_fullscreen(&mut self, window: WindowId, on: bool) {
        self.pin().set_fullscreen(window.0, on);
    }

    pub fn close(&mut self, window: WindowId) {
        self.pin().close(window.0);
    }

    pub fn title(&self, window: WindowId) -> String {
        self.raw.window_title(window.0)
    }

    pub fn app_id(&self, window: WindowId) -> String {
        self.raw.window_app_id(window.0)
    }

    pub fn size(&self, window: WindowId) -> (i32, i32) {
        let s = self.raw.window_size(window.0);
        (s.width, s.height)
    }

    pub fn min_size(&self, window: WindowId) -> (i32, i32) {
        let s = self.raw.window_min_size(window.0);
        (s.width, s.height)
    }

    pub fn parent(&self, window: WindowId) -> Option<WindowId> {
        match self.raw.window_parent(window.0) {
            0 => None,
            id => Some(WindowId(id)),
        }
    }

    pub fn icon_name(&self, window: WindowId) -> String {
        self.raw.window_icon_name(window.0)
    }

    pub fn set_minimized(&mut self, window: WindowId, on: bool) {
        self.pin().set_minimized(window.0, on);
    }

    pub fn set_tiled(&mut self, window: WindowId, on: bool) {
        self.pin().set_tiled(window.0, on);
    }

    pub fn set_resizing(&mut self, window: WindowId, on: bool) {
        self.pin().set_resizing(window.0, on);
    }

    pub fn grab_pointer(&mut self, on: bool) {
        self.pin().grab_pointer(on);
    }

    pub fn set_keyboard_layout(&mut self, layout: &str) -> bool {
        self.pin().set_keyboard_layout(layout)
    }

    pub fn output_box(&self, output: OutputId) -> Rect {
        self.raw.output_box(output.0)
    }

    pub fn output_usable(&self, output: OutputId) -> Rect {
        self.raw.output_usable(output.0)
    }

    pub fn output_modes(&self, output: OutputId) -> Vec<Mode> {
        self.raw.output_modes(output.0)
    }

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

    pub fn set_output_scale(&mut self, output: OutputId, scale: i32) {
        self.pin().set_output_scale(output.0, scale.max(1));
    }

    pub fn set_output_transform(&mut self, output: OutputId, transform: Transform) {
        self.pin().set_output_transform(output.0, transform as u32);
    }

    pub fn set_output_position(&mut self, output: OutputId, x: i32, y: i32) {
        self.pin().set_output_position(output.0, x, y);
    }

    pub fn add_workspace(&mut self, name: &str) -> WorkspaceId {
        WorkspaceId(self.pin().add_workspace(name))
    }

    pub fn remove_workspace(&mut self, workspace: WorkspaceId) {
        self.pin().remove_workspace(workspace.0);
    }

    pub fn activate_workspace(&mut self, workspace: WorkspaceId) {
        self.pin().activate_workspace(workspace.0);
    }

    pub fn is_locked(&self) -> bool {
        self.raw.is_locked()
    }

    pub fn bind_key(&mut self, mods: Mods, keysym: u32) -> BindingId {
        BindingId(self.pin().bind_key(mods.0, keysym))
    }

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
    fn as_raw_fd(&self) -> RawFd {
        self.raw.fd()
    }
}

pub fn keysym(name: &str) -> u32 {
    ffi::keysym_from_name(name)
}
