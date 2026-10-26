//! minimal — the smallest compositor that is still usable, and the Rust twin of
//! examples/minimal.cpp: every window is maximized onto the output, the newest
//! on top, like a phone. Clicking a window raises it.
//!
//!   cargo run --example minimal                                       # nested in the current session
//!   LUMINARIA_BACKEND=headless LUMINARIA_EXIT_MS=500 cargo run --example minimal
//!
//! LUMINARIA_BACKEND is auto (nested, then headless) | nested | headless.
//! Logo+Return opens $TERMINAL (default foot), Logo+Tab brings the bottom
//! window to the top, Logo+q closes the top one, Logo+Escape quits.
//!
//! Unlike its twin it has no DRM path: the binding does not report the outputs
//! a `DrmBackend` finds, nor the input `LibinputBackend` reads.
//!
//! What is left here is what a compositor decides: where windows go, which one
//! has the keyboard, what a key does. The pointer's picture is `Cursor`'s, and
//! answering the display — frame callbacks, the frame that turned out
//! unchanged — is `SceneRenderer`'s.

use std::process::Command;
use std::time::{Duration, Instant};

use luminaria::backend::{BackendInputEvent, HeadlessBackend, Output, WaylandBackend};
use luminaria::core::{Display, KeyRouter};
use luminaria::gpu::{Cursor, SceneOutput, SceneRenderer, VulkanOptions};
use luminaria::protocol::{
    Compositor, DataDeviceManager, FractionalScaleManager, OutputGlobal, Presentation, Seat,
    SinglePixelBufferManager, Subcompositor, SurfaceId, ToplevelEvent, Viewporter,
    XdgDecorationManager, XdgShell,
};
use luminaria::shell::{scene_hit_test, OutputLayout, SceneItem};
use luminaria::util::{Color, KeymapState, Rect};

const WIDTH: i32 = 1280;
const HEIGHT: i32 = 800;
const BACKGROUND: Color = Color::rgb(0.12, 0.13, 0.16);
const DRM_FORMAT_XRGB8888: u32 = 0x3432_5258;

// Shift, Control, Alt (Mod1), Logo (Mod4) in xkb's core modifier bits. Caps
// and Num Lock are left out: a binding must not stop working because a lock
// is on.
const BINDABLE_MODS: u32 = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 6);
const LOGO: u32 = 1 << 6;

#[derive(Clone, Copy)]
enum Action {
    Spawn,
    Cycle,
    Close,
    Quit,
}

// (modifiers, keysym, action); keysyms from xkbcommon-keysyms.h.
const BINDINGS: [(u32, u32, Action); 4] = [
    (LOGO, 0xff0d /* Return */, Action::Spawn),
    (LOGO, 0xff09 /* Tab */, Action::Cycle),
    (LOGO, 0x0071 /* q */, Action::Close),
    (LOGO, 0xff1b /* Escape */, Action::Quit),
];

enum Backend {
    Nested(WaylandBackend),
    Headless(#[allow(dead_code)] HeadlessBackend),
}

fn start(display: &Display) -> Result<(Backend, Output), luminaria::core::Error> {
    let want = std::env::var("LUMINARIA_BACKEND").unwrap_or_else(|_| "auto".into());
    if want != "headless" {
        match WaylandBackend::new(display) {
            Ok(mut nested) => {
                let output = nested.add_output(WIDTH, HEIGHT)?;
                nested.start()?;
                return Ok((Backend::Nested(nested), output));
            }
            Err(e) if want == "nested" => return Err(e),
            Err(_) => {}
        }
    }
    let mut headless = HeadlessBackend::new(display, 16)?;
    let output = headless.add_output(WIDTH, HEIGHT)?;
    headless.start()?;
    Ok((Backend::Headless(headless), output))
}

/// Every window covers the whole output. Tagged with its own surface, so a
/// hit on one of its subsurfaces still names the window.
fn scene(stack: &[SurfaceId], screen: Rect) -> Vec<SceneItem> {
    stack
        .iter()
        .map(|&s| SceneItem::surface(s, screen, screen.x, screen.y).with_tag(s.0))
        .collect()
}

/// The top window has the keyboard; every other one is told it lost it.
fn focus_top(stack: &[SurfaceId], shell: &mut XdgShell, seat: &mut Seat) {
    for (i, &s) in stack.iter().enumerate() {
        shell.set_activated(s, i + 1 == stack.len());
    }
    seat.set_keyboard_focus(stack.last().copied());
}

fn raise(stack: &mut Vec<SurfaceId>, surface: SurfaceId) {
    stack.retain(|&s| s != surface);
    stack.push(surface);
}

/// Gives the pointer to whatever is under it. A held button keeps it on the
/// surface it went down on; every window sits at the output's origin, so the
/// pointer's coordinates are that window's own.
fn pointer_focus(seat: &mut Seat, stack: &[SurfaceId], screen: Rect, (x, y): (f64, f64)) {
    if seat.pointer_grab().is_some() {
        seat.pointer_motion(x - screen.x as f64, y - screen.y as f64);
        return;
    }
    let hit = scene_hit_test(&scene(stack, screen), x, y);
    match hit.surface {
        Some(surface) => seat.pointer_enter(surface, hit.sx, hit.sy),
        None if seat.pointer_focus().is_some() => seat.pointer_clear_focus(),
        None => {}
    }
}

fn binding(keymap: &KeymapState, code: u32) -> Option<Action> {
    let (depressed, latched, _, _) = keymap.modifiers();
    let mods = (depressed | latched) & BINDABLE_MODS;
    let sym = keymap.keysym(code);
    BINDINGS
        .iter()
        .find(|&&(m, s, _)| m == mods && s == sym)
        .map(|&(_, _, action)| action)
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Declaration order is destruction order, reversed. The renderer first, so
    // it dies last: the textures it caches are dropped along with the Display.
    let mut renderer = SceneRenderer::new(&VulkanOptions::default())?;
    let display = Display::new()?;
    let socket = display.add_socket_auto()?;
    display.init_shm()?;
    println!("minimal: WAYLAND_DISPLAY={socket}");

    let (mut backend, mut output) = start(&display)?;
    println!(
        "minimal: backend = {}",
        match backend {
            Backend::Nested(_) => "nested",
            Backend::Headless(_) => "headless",
        }
    );
    if renderer.gpu_enabled() {
        println!("minimal: renderer = GPU");
    } else {
        println!("minimal: renderer = CPU ({})", renderer.gpu_error());
    }

    let _compositor = Compositor::new(&display)?;
    let _subcompositor = Subcompositor::new(&display)?;
    let mut shell = XdgShell::new(&display)?;
    shell.set_bounds(WIDTH, HEIGHT);
    let mut seat = Seat::new(&display, "seat0")?;
    seat.set_capabilities(true, true, false);
    let _data_device = DataDeviceManager::new(&display, &seat)?;
    let _single_pixel = SinglePixelBufferManager::new(&display)?;
    let presentation = Presentation::new(&display)?;
    let _viewporter = Viewporter::new(&display)?;
    let _fractional = FractionalScaleManager::new(&display)?;
    let _decoration = XdgDecorationManager::new(&display)?;
    renderer.set_presentation(Some(&presentation));
    let _output_global = OutputGlobal::new(&display, WIDTH, HEIGHT, "minimal-1")?;

    // The host's layout: bindings are matched against it, and clients should
    // see the layout the user types on.
    let mut keymap: Option<KeymapState> = None;
    if let Backend::Nested(nested) = &backend {
        let text = nested.keymap();
        if let Ok(k) = KeymapState::from_text(&text) {
            if seat.set_keymap(&text) {
                keymap = Some(k);
            }
        }
    }

    let mut layout = OutputLayout::new()?;
    layout.add(&mut output, 0, 0);
    let screen = layout.output_box(&output);

    let mut state = SceneOutput::new();
    renderer.reset(&mut state, &mut output, DRM_FORMAT_XRGB8888)?;
    let mut cursor = Cursor::new(&mut renderer, &layout, &mut seat)?;

    let mut stack: Vec<SurfaceId> = Vec::new(); // bottom to top
    let mut keys = KeyRouter::new();
    let mut pointer: Option<(f64, f64)> = None;
    output.schedule_frame();

    let deadline = std::env::var("LUMINARIA_EXIT_MS")
        .ok()
        .and_then(|ms| ms.parse().ok())
        .map(|ms| Instant::now() + Duration::from_millis(ms));

    'run: loop {
        display.dispatch(Some(Duration::from_millis(16)))?;
        display.flush_clients();
        if deadline.is_some_and(|d| Instant::now() >= d) {
            break;
        }

        let mut restacked = false;
        while let Some(event) = shell.next_event() {
            match event {
                ToplevelEvent::Created(surface) => {
                    shell.set_maximized(surface, true);
                    shell.configure(surface, screen.width, screen.height);
                }
                ToplevelEvent::Map(surface) => {
                    raise(&mut stack, surface);
                    restacked = true;
                }
                ToplevelEvent::Unmap(surface) | ToplevelEvent::Destroy(surface) => {
                    stack.retain(|&s| s != surface);
                    restacked = true;
                }
            }
        }

        if let Backend::Nested(nested) = &mut backend {
            while let Some(event) = nested.next_input_event() {
                match event {
                    BackendInputEvent::Motion { x, .. } if x < 0.0 => {
                        // Left the host window.
                        pointer = None;
                        cursor.leave();
                        seat.pointer_clear_focus();
                    }
                    BackendInputEvent::Motion { x, y } => {
                        let at = (screen.x as f64 + x, screen.y as f64 + y);
                        pointer = Some(at);
                        cursor.move_to(at.0, at.1, screen);
                        pointer_focus(&mut seat, &stack, screen, at);
                    }
                    BackendInputEvent::Button { button, pressed } => {
                        if pressed {
                            if let Some((x, y)) = pointer {
                                let hit = scene_hit_test(&scene(&stack, screen), x, y);
                                if hit.tag != 0 && stack.last() != Some(&SurfaceId(hit.tag)) {
                                    raise(&mut stack, SurfaceId(hit.tag));
                                    restacked = true;
                                }
                            }
                        }
                        if seat.pointer_focus().is_some() {
                            seat.pointer_button(button, pressed);
                        }
                        if let (false, Some(at)) = (pressed, pointer) {
                            pointer_focus(&mut seat, &stack, screen, at); // a grab may have ended
                        }
                    }
                    BackendInputEvent::Key { key, pressed } => {
                        if let Some(k) = keymap.as_mut() {
                            k.update_key(key, pressed);
                            let (depressed, latched, locked, group) = k.modifiers();
                            seat.notify_modifiers(depressed, latched, locked, group);
                        }
                        if !pressed {
                            if keys.unconsume(key) {
                                continue; // the release of a key we kept stays ours
                            }
                            seat.notify_key(key, false);
                            continue;
                        }
                        // A press of a key we think we still hold: its release was lost.
                        keys.unconsume(key);
                        let Some(action) = keymap.as_ref().and_then(|k| binding(k, key)) else {
                            seat.notify_key(key, true);
                            continue;
                        };
                        keys.consume(key);
                        match action {
                            Action::Spawn => {
                                let term =
                                    std::env::var("TERMINAL").unwrap_or_else(|_| "foot".into());
                                if let Err(e) = Command::new(&term).spawn() {
                                    eprintln!("minimal: {term}: {e}");
                                }
                            }
                            Action::Cycle if stack.len() > 1 => {
                                stack.rotate_left(1);
                                restacked = true;
                            }
                            Action::Cycle => {}
                            Action::Close => {
                                if let Some(&top) = stack.last() {
                                    shell.close(top);
                                }
                            }
                            Action::Quit => break 'run,
                        }
                    }
                }
            }
        }

        // Nothing a client reports changed, so the output is asked for a frame
        // and the frame's diff works out what moved.
        if restacked {
            focus_top(&stack, &mut shell, &mut seat);
            if let Some(at) = pointer {
                pointer_focus(&mut seat, &stack, screen, at);
            }
            output.schedule_frame();
        }

        if output.take_frame() {
            if let Err(e) = renderer.present(&mut state, &mut output, &scene(&stack, screen), BACKGROUND) {
                eprintln!("minimal: present: {e}");
            }
        }
    }

    Ok(())
}
