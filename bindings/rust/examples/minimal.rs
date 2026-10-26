//! The smallest compositor that is still usable: every window is maximized
//! into the area panels leave free, the newest on top, like a phone. Popups,
//! panels, the cursor, the clipboard and the rest are the library's job.
//!
//!   cargo run --example minimal                         # nested in the current session
//!   LUMINARIA_BACKEND=drm cargo run --example minimal   # bare metal, from a free VT
//!
//! Logo+Return opens $TERMINAL (default foot), Logo+Tab brings the bottom
//! window to the top, Logo+q closes the top one, Logo+Escape quits. Clicking
//! a window (or a taskbar entry) raises it. One output is all it lays out on.
//!
//! The C++ twin is `examples/minimal.cpp` at the repository root: the same
//! policy and bindings, so a change here belongs there too.

use std::ops::ControlFlow;
use std::process::Command;

use luminaria::{Backend, Config, Event, Item, Rect, Server, WindowId};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let backend = match std::env::var("LUMINARIA_BACKEND").as_deref() {
        Ok("headless") => Backend::Headless,
        Ok("nested") => Backend::Nested,
        Ok("drm") => Backend::Drm,
        _ => Backend::Auto,
    };
    let mut server = Server::new(Config {
        backend,
        ..Config::default()
    })?;
    let socket = server.socket_name();
    println!("minimal: WAYLAND_DISPLAY={socket}");

    let spawn = server.bind("Logo+Return")?;
    let cycle = server.bind("Logo+Tab")?;
    let close = server.bind("Logo+q")?;
    let quit = server.bind("Logo+Escape")?;

    let mut usable = Rect::new(0, 0, 1280, 800);
    let mut stack: Vec<WindowId> = Vec::new(); // mapped windows, back to front

    server.run(|server, event| {
        let layout_only = matches!(event, Event::OutputNew { .. } | Event::OutputChange(_));
        match event {
            Event::OutputNew { output, .. } | Event::OutputChange(output) => {
                usable = server.output_usable(output);
                for &w in &stack {
                    server.configure(w, usable.width, usable.height);
                }
            }
            // Sized before its first frame, so it never shows at its own size.
            Event::WindowNew(w) => {
                server.set_maximized(w, true);
                server.configure(w, usable.width, usable.height);
                return ControlFlow::Continue(());
            }
            Event::WindowMap(w) => stack.push(w),
            Event::WindowUnmap(w) | Event::WindowDestroy(w) => stack.retain(|&s| s != w),
            Event::PointerButton {
                window: Some(w),
                pressed: true,
                ..
            }
            | Event::RequestActivate(w) => {
                stack.retain(|&s| s != w);
                stack.push(w);
            }
            Event::Binding(b) if b == cycle => stack.rotate_left(1),
            Event::Binding(b) if b == close => {
                if let Some(&w) = stack.last() {
                    server.close(w);
                }
                return ControlFlow::Continue(());
            }
            Event::Binding(b) if b == spawn => {
                let term = std::env::var("TERMINAL").unwrap_or_else(|_| "foot".into());
                if let Err(e) = Command::new(&term).env("WAYLAND_DISPLAY", &socket).spawn() {
                    eprintln!("minimal: {term}: {e}");
                }
                return ControlFlow::Continue(());
            }
            Event::Binding(b) if b == quit => return ControlFlow::Break(()),
            Event::CloseRequested => return ControlFlow::Break(()),
            // Pointer motion and the rest change nothing here.
            _ => return ControlFlow::Continue(()),
        }
        // The top window has the keyboard. Asking takes it back from a panel
        // that grabbed it, so a layout change alone does not ask.
        if !layout_only {
            server.focus(stack.last().copied());
        }
        let scene: Vec<Item> = stack
            .iter()
            .map(|&id| Item::Window {
                id,
                x: usable.x,
                y: usable.y,
            })
            .collect();
        server.set_scene(&scene);
        ControlFlow::Continue(())
    })?;
    Ok(())
}
