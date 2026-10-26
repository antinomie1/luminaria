//! A floating window manager in ~100 lines: cascade new windows, click to
//! focus and raise, drag by the title bar, maximize on request.
//!
//!   cargo run --example floating            # nested in the current session
//!   LUMINARIA_EXIT_MS=500 cargo run --example floating   # smoke test
//!
//! Logo+Return opens $TERMINAL (default foot), Logo+q closes the focused
//! window, Logo+Escape quits.

use std::ops::ControlFlow;
use std::process::Command;
use std::time::{Duration, Instant};

use luminaria::{Backend, Color, Config, Event, Item, Server, WindowId};

struct Window {
    id: WindowId,
    x: i32,
    y: i32,
    mapped: bool,
    saved: Option<(i32, i32)>, // position before maximize
}

#[derive(Default)]
struct Wm {
    windows: Vec<Window>, // back to front
    focused: Option<WindowId>,
    drag: Option<(WindowId, f64, f64)>, // window, grab offset
    pointer: (f64, f64),
}

impl Wm {
    fn scene(&self) -> Vec<Item> {
        let mut scene = Vec::new();
        for w in self.windows.iter().filter(|w| w.mapped) {
            scene.push(Item::Window {
                id: w.id,
                x: w.x,
                y: w.y,
            });
        }
        scene
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
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let backend = match std::env::var("LUMINARIA_BACKEND").as_deref() {
        Ok("headless") => Backend::Headless,
        _ => Backend::Auto,
    };
    let mut server = Server::new(Config {
        backend,
        ..Config::default()
    })?;
    let socket = server.socket_name();
    println!("floating: WAYLAND_DISPLAY={socket}");
    server.set_background(Color::rgb(0.12, 0.13, 0.16));

    let spawn = server.bind("Logo+Return")?;
    let close = server.bind("Logo+q")?;
    let quit = server.bind("Logo+Escape")?;
    let exit_at = std::env::var("LUMINARIA_EXIT_MS")
        .ok()
        .and_then(|ms| ms.parse().ok())
        .map(|ms| Instant::now() + Duration::from_millis(ms));

    let mut wm = Wm::default();
    let mut output = (1280, 800);
    loop {
        server.dispatch(Some(Duration::from_millis(100)))?;
        if exit_at.is_some_and(|t| Instant::now() >= t) {
            return Ok(());
        }
        while let Some(event) = server.next_event() {
            if handle(
                &mut server,
                &mut wm,
                &mut output,
                event,
                &socket,
                [spawn, close, quit],
            )
            .is_break()
            {
                return Ok(());
            }
            server.set_scene(&wm.scene());
        }
    }
}

fn handle(
    server: &mut Server,
    wm: &mut Wm,
    output: &mut (i32, i32),
    event: Event,
    socket: &str,
    [spawn, close, quit]: [luminaria::BindingId; 3],
) -> ControlFlow<()> {
    match event {
        Event::OutputNew { width, height, .. } => *output = (width, height),
        Event::WindowNew(id) => {
            let n = wm.windows.len() as i32;
            wm.windows.push(Window {
                id,
                x: 40 + 30 * n,
                y: 40 + 30 * n,
                mapped: false,
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
            wm.raise(id);
            wm.focused = Some(id);
            server.focus(Some(id));
        }
        Event::WindowUnmap(id) => {
            if let Some(w) = wm.get(id) {
                w.mapped = false;
            }
        }
        Event::WindowDestroy(id) => {
            wm.windows.retain(|w| w.id != id);
            if wm.focused == Some(id) {
                wm.focused = wm.windows.iter().rev().find(|w| w.mapped).map(|w| w.id);
                server.focus(wm.focused);
            }
        }
        Event::RequestMaximize { window, on } => {
            let (ow, oh) = *output;
            if let Some(w) = wm.get(window) {
                if on {
                    w.saved = Some((w.x, w.y));
                    (w.x, w.y) = (0, 0);
                } else if let Some((x, y)) = w.saved.take() {
                    (w.x, w.y) = (x, y);
                }
            }
            server.set_maximized(window, on);
            server.configure(window, if on { ow } else { 0 }, if on { oh } else { 0 });
        }
        // Sent by the client right after the press on its title bar.
        Event::RequestMove(id) => {
            let (px, py) = wm.pointer;
            if let Some(w) = wm.get(id) {
                wm.drag = Some((id, px - w.x as f64, py - w.y as f64));
            }
        }
        Event::PointerButton {
            window,
            button: 272,
            pressed,
            ..
        } => {
            if !pressed {
                wm.drag = None;
            } else if let Some(id) = window {
                wm.raise(id);
                wm.focused = Some(id);
                server.focus(Some(id));
            }
        }
        Event::PointerMotion { x, y, .. } => {
            wm.pointer = (x, y);
            if let Some((id, dx, dy)) = wm.drag
                && let Some(w) = wm.get(id)
            {
                (w.x, w.y) = ((x - dx) as i32, (y - dy) as i32);
            }
        }
        Event::Binding(b) if b == spawn => {
            let term = std::env::var("TERMINAL").unwrap_or_else(|_| "foot".into());
            if let Err(e) = Command::new(&term).env("WAYLAND_DISPLAY", socket).spawn() {
                eprintln!("floating: {term}: {e}");
            }
        }
        Event::Binding(b) if b == close => {
            if let Some(id) = wm.focused {
                server.close(id);
            }
        }
        Event::Binding(b) if b == quit => return ControlFlow::Break(()),
        Event::CloseRequested => return ControlFlow::Break(()),
        _ => {}
    }
    ControlFlow::Continue(())
}
