//! A floating window manager: cascade new windows, click to focus and raise,
//! drag by the title bar, resize from the edges, maximize into the area panels
//! leave free, minimize, two workspaces.
//!
//!   cargo run --example floating            # nested in the current session
//!   LUMINARIA_EXIT_MS=500 cargo run --example floating   # smoke test
//!   LUMINARIA_BACKEND=drm cargo run --example floating   # bare metal, from a free VT
//!
//! LUMINARIA_BACKEND is auto|nested|headless|drm; LUMINARIA_DRM_DEVICE picks
//! the card; LUMINARIA_LOG_EVENTS=1 prints every event but pointer motion.
//! On bare metal Ctrl+Alt+Fn still switches VTs.
//!
//! Logo+Return opens $TERMINAL (default foot), Logo+q closes the focused
//! window, Logo+1 / Logo+2 switch workspaces, Logo+Escape quits.

use std::ops::ControlFlow;
use std::process::Command;
use std::time::{Duration, Instant};

use luminaria::{
    Backend, BindingId, Color, Config, Event, Item, Rect, Server, WindowId, WorkspaceId,
};

struct Window {
    id: WindowId,
    x: i32,
    y: i32,
    mapped: bool,
    minimized: bool,
    workspace: usize,
    saved: Option<(i32, i32)>, // position before maximize
}

enum Grab {
    Move {
        id: WindowId,
        dx: f64,
        dy: f64,
    },
    Resize {
        id: WindowId,
        edges: u32,
        start: Rect,
        px: f64,
        py: f64,
    },
}

struct Wm {
    windows: Vec<Window>, // back to front
    focused: Option<WindowId>,
    grab: Option<Grab>,
    pointer: (f64, f64),
    usable: Rect, // where windows go: the output minus panels
    workspaces: [WorkspaceId; 2],
    current: usize,
    socket: String,
    keys: Keys,
}

struct Keys {
    spawn: BindingId,
    close: BindingId,
    quit: BindingId,
    ws: [BindingId; 2],
}

impl Wm {
    fn scene(&self) -> Vec<Item> {
        self.windows
            .iter()
            .filter(|w| w.mapped && !w.minimized && w.workspace == self.current)
            .map(|w| Item::Window {
                id: w.id,
                x: w.x,
                y: w.y,
            })
            .collect()
    }

    fn raise(&mut self, id: WindowId) {
        if let Some(i) = self.windows.iter().position(|w| w.id == id) {
            let w = self.windows.remove(i);
            self.windows.push(w);
        }
    }

    fn get(&mut self, id: WindowId) -> Option<&mut Window> {
        self.windows.iter_mut().find(|w| w.id == id)
    }

    fn focus(&mut self, server: &mut Server, id: Option<WindowId>) {
        if let Some(id) = id {
            self.raise(id);
        }
        self.focused = id;
        server.focus(id);
    }

    /// The topmost window someone could see, for when the focused one goes.
    fn refocus(&mut self, server: &mut Server) {
        let next = self
            .windows
            .iter()
            .rev()
            .find(|w| w.mapped && !w.minimized && w.workspace == self.current)
            .map(|w| w.id);
        self.focus(server, next);
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let backend = match std::env::var("LUMINARIA_BACKEND").as_deref() {
        Ok("headless") => Backend::Headless,
        Ok("nested") => Backend::Nested,
        Ok("drm") => Backend::Drm,
        _ => Backend::Auto,
    };
    let mut server = Server::new(Config {
        backend,
        drm_device: std::env::var("LUMINARIA_DRM_DEVICE").ok(),
        ..Config::default()
    })?;
    let socket = server.socket_name();
    println!("floating: WAYLAND_DISPLAY={socket}");
    server.set_background(Color::rgb(0.12, 0.13, 0.16));

    let keys = Keys {
        spawn: server.bind("Logo+Return")?,
        close: server.bind("Logo+q")?,
        quit: server.bind("Logo+Escape")?,
        ws: [server.bind("Logo+1")?, server.bind("Logo+2")?],
    };
    let workspaces = [server.add_workspace("1"), server.add_workspace("2")];
    server.activate_workspace(workspaces[0]);

    let exit_at = std::env::var("LUMINARIA_EXIT_MS")
        .ok()
        .and_then(|ms| ms.parse().ok())
        .map(|ms| Instant::now() + Duration::from_millis(ms));
    let log_events = std::env::var_os("LUMINARIA_LOG_EVENTS").is_some();

    let mut wm = Wm {
        windows: Vec::new(),
        focused: None,
        grab: None,
        pointer: (0.0, 0.0),
        usable: Rect::new(0, 0, 1280, 800),
        workspaces,
        current: 0,
        socket,
        keys,
    };
    loop {
        server.dispatch(Some(Duration::from_millis(100)))?;
        if exit_at.is_some_and(|t| Instant::now() >= t) {
            return Ok(());
        }
        while let Some(event) = server.next_event() {
            if log_events
                && !matches!(
                    event,
                    Event::PointerMotion { .. } | Event::TouchMotion { .. }
                )
            {
                println!("floating: {event:?}");
            }
            if handle(&mut server, &mut wm, event).is_break() {
                return Ok(());
            }
            server.set_scene(&wm.scene());
        }
    }
}

fn handle(server: &mut Server, wm: &mut Wm, event: Event) -> ControlFlow<()> {
    match event {
        // One output is all this example lays out on.
        Event::OutputNew { output, .. } | Event::OutputChange(output) => {
            wm.usable = server.output_usable(output);
        }
        Event::WindowNew(id) => {
            let n = wm.windows.len() as i32;
            wm.windows.push(Window {
                id,
                x: wm.usable.x + 40 + 30 * n,
                y: wm.usable.y + 40 + 30 * n,
                mapped: false,
                minimized: false,
                workspace: wm.current,
                saved: None,
            });
        }
        Event::WindowMap(id) => {
            if let Some(w) = wm.get(id) {
                w.mapped = true;
            }
            println!(
                "floating: map {:?} {:?} ({})",
                id,
                server.size(id),
                server.app_id(id)
            );
            wm.focus(server, Some(id));
        }
        Event::WindowUnmap(id) => {
            if let Some(w) = wm.get(id) {
                w.mapped = false;
            }
        }
        Event::WindowDestroy(id) => {
            wm.windows.retain(|w| w.id != id);
            if wm.focused == Some(id) {
                wm.refocus(server);
            }
        }
        Event::RequestMaximize { window, on } => {
            let usable = wm.usable;
            if let Some(w) = wm.get(window) {
                if on {
                    w.saved = Some((w.x, w.y));
                    (w.x, w.y) = (usable.x, usable.y);
                } else if let Some((x, y)) = w.saved.take() {
                    (w.x, w.y) = (x, y);
                }
            }
            server.set_maximized(window, on);
            let (w, h) = if on {
                (usable.width, usable.height)
            } else {
                (0, 0)
            };
            server.configure(window, w, h);
        }
        Event::RequestMinimize { window, on } => {
            if let Some(w) = wm.get(window) {
                w.minimized = on;
            }
            server.set_minimized(window, on);
            if on && wm.focused == Some(window) {
                wm.refocus(server);
            } else if !on {
                wm.focus(server, Some(window));
            }
        }
        Event::RequestActivate(window) => {
            if let Some(w) = wm.get(window) {
                w.minimized = false;
            }
            wm.focus(server, Some(window));
        }
        // Both arrive right after the press on the title bar or the edge.
        Event::RequestMove(id) => {
            let (px, py) = wm.pointer;
            if let Some(w) = wm.get(id) {
                let (dx, dy) = (px - w.x as f64, py - w.y as f64);
                wm.grab = Some(Grab::Move { id, dx, dy });
                server.grab_pointer(true);
            }
        }
        Event::RequestResize { window, edges } => {
            let (px, py) = wm.pointer;
            let (width, height) = server.size(window);
            if let Some(w) = wm.get(window) {
                let start = Rect::new(w.x, w.y, width, height);
                wm.grab = Some(Grab::Resize {
                    id: window,
                    edges,
                    start,
                    px,
                    py,
                });
                server.grab_pointer(true);
                server.set_resizing(window, true);
            }
        }
        Event::PointerButton {
            window,
            button: 272,
            pressed,
            ..
        } => {
            if !pressed {
                if let Some(Grab::Resize { id, .. }) = wm.grab {
                    server.set_resizing(id, false);
                }
                if wm.grab.take().is_some() {
                    server.grab_pointer(false);
                }
            } else if let Some(id) = window {
                wm.focus(server, Some(id));
            }
        }
        // A tap focuses like a click; the client under the finger already has it.
        Event::TouchDown {
            window: Some(id), ..
        } => wm.focus(server, Some(id)),
        Event::PointerMotion { x, y, .. } => {
            wm.pointer = (x, y);
            match wm.grab {
                Some(Grab::Move { id, dx, dy }) => {
                    if let Some(w) = wm.get(id) {
                        (w.x, w.y) = ((x - dx) as i32, (y - dy) as i32);
                    }
                }
                Some(Grab::Resize {
                    id,
                    edges,
                    start,
                    px,
                    py,
                }) => {
                    let min = server.min_size(id);
                    let r = start.resized(edges, (x - px) as i32, (y - py) as i32, min);
                    server.configure(id, r.width, r.height);
                    if let Some(w) = wm.get(id) {
                        (w.x, w.y) = (r.x, r.y);
                    }
                }
                None => {}
            }
        }
        Event::Binding(b) if b == wm.keys.spawn => {
            let term = std::env::var("TERMINAL").unwrap_or_else(|_| "foot".into());
            if let Err(e) = Command::new(&term)
                .env("WAYLAND_DISPLAY", &wm.socket)
                .spawn()
            {
                eprintln!("floating: {term}: {e}");
            }
        }
        Event::Binding(b) if b == wm.keys.close => {
            if let Some(id) = wm.focused {
                server.close(id);
            }
        }
        Event::Binding(b) if b == wm.keys.quit => return ControlFlow::Break(()),
        Event::Binding(b) if wm.keys.ws.contains(&b) => {
            let n = wm.keys.ws.iter().position(|&k| k == b).unwrap_or(0);
            switch_workspace(server, wm, n);
        }
        // A pager asked.
        Event::WorkspaceActivate(ws) => {
            if let Some(n) = wm.workspaces.iter().position(|&w| w == ws) {
                switch_workspace(server, wm, n);
            }
        }
        Event::CloseRequested => return ControlFlow::Break(()),
        _ => {}
    }
    ControlFlow::Continue(())
}

fn switch_workspace(server: &mut Server, wm: &mut Wm, n: usize) {
    if n != wm.current {
        wm.current = n;
        server.activate_workspace(wm.workspaces[n]);
        wm.refocus(server);
    }
}
