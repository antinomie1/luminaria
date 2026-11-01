//! minimal — the smallest compositor that is still usable, and the Rust twin of
//! examples/minimal.cpp: every window fills the first output, newest on top. What
//! is left here is what a compositor decides: where windows go, which one has the
//! keyboard, what a key does. Logo+Return opens $TERMINAL (default foot), Logo+Tab
//! brings the bottom window up, Logo+q closes the top one, Logo+Escape quits.
//! LUMINARIA_BACKEND=auto|nested|drm|headless, LUMINARIA_DRM_DEVICE (or the first
//! argument), LUMINARIA_VRR=1 and LUMINARIA_EXIT_MS say how it runs.

use std::process::Command;
use std::time::{Duration, Instant};

use luminaria::Event;
use luminaria::backend::{AutoBackend, BackendOptions, Modifiers};
use luminaria::core::Display;
use luminaria::gpu::{Cursor, SceneRenderer};
use luminaria::protocol::{CoreGlobals, SurfaceId, ToplevelEvent};
use luminaria::shell::{
    InputRouter, OutputLayout, PopupTree, SceneItem, ScreenManager, ScreenOptions,
};
use luminaria::util::{Color, Keysym, Rect};

const SPAWN: u32 = 0;
const CYCLE: u32 = 1;
const CLOSE: u32 = 2;
const QUIT: u32 = 3;

fn raise(stack: &mut Vec<SurfaceId>, surface: SurfaceId) {
    stack.retain(|&s| s != surface);
    stack.push(surface);
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut options = BackendOptions::from_env(std::env::args().nth(1))?;
    options.title = "minimal".into();

    let display = Display::new()?;
    let globals = CoreGlobals::new(&display)?;
    let (shell, seat) = (globals.shell(), globals.seat());
    let layout = OutputLayout::new()?;
    let popups = PopupTree::new(shell)?;
    let backend = AutoBackend::new(&display, &options)?;

    let renderer = SceneRenderer::new(&backend.vulkan_options())?;
    renderer.set_presentation(Some(globals.presentation()));
    let _dmabuf = renderer.linux_dmabuf(&display);
    let cursor = Cursor::new_with_shapes(&renderer, &layout, seat, globals.cursor_shape())?;

    let screen_options = ScreenOptions {
        background: Color::rgb(0.12, 0.13, 0.16),
        variable_refresh: std::env::var("LUMINARIA_VRR").is_ok_and(|v| !v.is_empty() && v != "0"),
        name: "minimal".into(),
        ..ScreenOptions::default()
    };
    let screens = ScreenManager::new(&display, &backend, &renderer, &layout, &screen_options)?;
    screens.follow_popups(&popups);
    let input = InputRouter::new(seat, &cursor, &screens, shell, &popups)?;
    input.attach(&backend);
    input.bind(Modifiers::LOGO, Keysym::Return, SPAWN);
    input.bind(Modifiers::LOGO, Keysym::Tab, CYCLE);
    input.bind(Modifiers::LOGO, Keysym::q, CLOSE);
    input.bind(Modifiers::LOGO, Keysym::Escape, QUIT);

    let socket = display.add_socket_auto()?;
    backend.start()?;
    println!("minimal: WAYLAND_DISPLAY={socket}");
    let deadline = std::env::var("LUMINARIA_EXIT_MS")
        .ok()
        .and_then(|ms| ms.parse().ok())
        .map(|ms| Instant::now() + Duration::from_millis(ms));

    let mut stack: Vec<SurfaceId> = Vec::new(); // mapped windows, back to front
    let mut usable = Rect::new(0, 0, options.width, options.height); // the first output
    while deadline.is_none_or(|d| Instant::now() < d) {
        let top = stack.last().copied();
        for event in display.dispatch(Some(Duration::from_millis(16)))? {
            match event {
                Event::LayoutChanged if layout.bounds().width > 0 => {
                    usable = layout.primary_box();
                    shell.set_bounds(usable.size());
                    for t in stack.iter().filter_map(|&s| shell.toplevel(s)) {
                        t.configure(usable.size());
                    }
                }
                Event::Toplevel(surface, ToplevelEvent::Created) => {
                    if let Some(t) = shell.toplevel(surface) {
                        t.set_maximized(true);
                        t.configure(usable.size()); // before its first frame
                    }
                }
                Event::Toplevel(surface, ToplevelEvent::Map) => {
                    let scale = f64::from(layout.primary_scale());
                    globals.fractional_scale().set_scale(surface, scale);
                    raise(&mut stack, surface);
                }
                Event::Toplevel(surface, ToplevelEvent::Unmap | ToplevelEvent::Destroy) => {
                    stack.retain(|&s| s != surface);
                }
                Event::Button {
                    pressed: true, hit, ..
                } if hit.tag != 0 => raise(&mut stack, SurfaceId(hit.tag)),
                // Through a shell that exits at once, so the terminal is nobody's
                // child and leaves no zombie behind.
                Event::KeyBinding(SPAWN) => {
                    let spawn = ["-c", "${TERMINAL:-foot} &"];
                    let _ = Command::new("sh")
                        .args(spawn)
                        .env("WAYLAND_DISPLAY", &socket)
                        .status();
                }
                Event::KeyBinding(CYCLE) if stack.len() > 1 => stack.rotate_left(1),
                Event::KeyBinding(CLOSE) => {
                    if let Some(t) = stack.last().and_then(|&top| shell.toplevel(top)) {
                        t.close();
                    }
                }
                Event::KeyBinding(QUIT) | Event::HostCloseRequest => return Ok(()),
                Event::ScreenFailed(message) => eprintln!("minimal: {message}"),
                _ => {}
            }
        }
        // The top window has the keyboard and the activated look.
        if stack.last().copied() != top {
            shell.focus(seat, stack.last().copied());
        }
        let toplevels = stack.iter().filter_map(|&s| shell.toplevel(s));
        let mut scene: Vec<SceneItem> = toplevels
            .map(|t| SceneItem::toplevel(&t, usable.x, usable.y))
            .collect();
        popups.append_to(&mut scene);
        screens.set_scene(&scene);
    }
    Ok(())
}
