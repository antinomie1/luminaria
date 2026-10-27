//! minimal — the smallest compositor that is still usable, and the Rust twin of
//! examples/minimal.cpp: every window is maximized onto the output, the newest
//! on top, like a phone. Clicking a window raises it.
//!
//!   cargo run --example minimal                                       # nested in the current session
//!   LUMINARIA_BACKEND=drm cargo run --example minimal                 # bare metal, from a free VT
//!   LUMINARIA_BACKEND=headless LUMINARIA_EXIT_MS=500 cargo run --example minimal
//!
//! LUMINARIA_BACKEND is auto (nested, then DRM when a libseat session can be
//! had, then headless) | nested | drm | headless. On DRM the card is the first
//! argument or LUMINARIA_DRM_DEVICE, else the first one with a monitor;
//! Ctrl+Alt+Fn switches VTs. Logo+Return opens $TERMINAL (default foot),
//! Logo+Tab brings the bottom window to the top, Logo+q closes the top one,
//! Logo+Escape quits.
//!
//! What is left here is what a compositor decides: where windows go, which one
//! has the keyboard, what a key does.

use std::process::Command;
use std::time::{Duration, Instant};

use luminaria::Event;
use luminaria::backend::{AutoBackend, BackendOptions, InputEvent, Modifiers, Output, OutputEvent};
use luminaria::core::{Display, KeyRouter};
use luminaria::gpu::{Cursor, LinuxDmabuf, SceneRenderer};
use luminaria::protocol::{
    CoreGlobals, OutputGlobal, PopupArea, SurfaceId, ToplevelEvent, XdgShell,
};
use luminaria::shell::{OutputLayout, PopupTree, SceneItem, scene_hit_test, scene_route_pointer};
use luminaria::util::{Color, Keysym, Rect};

const BACKGROUND: Color = Color::rgb(0.12, 0.13, 0.16);

#[derive(Clone, Copy)]
enum Action {
    Spawn,
    Cycle,
    Close,
    Quit,
}

const BINDINGS: [(u32, Keysym, Action); 4] = [
    (Modifiers::LOGO, Keysym::Return, Action::Spawn),
    (Modifiers::LOGO, Keysym::Tab, Action::Cycle),
    (Modifiers::LOGO, Keysym::q, Action::Close),
    (Modifiers::LOGO, Keysym::Escape, Action::Quit),
];

/// Every window sits at the usable area's origin, newest on top, with its
/// popups above it. Tagged with its own surface, so a hit on one of its
/// subsurfaces or popups still names the window.
fn scene(
    shell: &XdgShell,
    popups: &PopupTree,
    stack: &[SurfaceId],
    usable: Rect,
) -> Vec<SceneItem> {
    let mut items: Vec<SceneItem> = stack
        .iter()
        .filter_map(|&s| {
            // The client's shadow sits outside its window geometry, so the
            // geometry, not the buffer, is what lands on the usable area.
            let g = shell.toplevel(s)?.geometry();
            let framed = g.width > 0 && g.height > 0;
            let (w, h) = if framed {
                (g.width, g.height)
            } else {
                (s.surface_width(), s.surface_height())
            };
            let (ox, oy) = if framed { (g.x, g.y) } else { (0, 0) };
            Some(
                SceneItem::surface(
                    s,
                    Rect::new(usable.x, usable.y, w, h),
                    usable.x - ox,
                    usable.y - oy,
                )
                .with_tag(s.0),
            )
        })
        .collect();
    popups.append_to(&mut items);
    items
}

/// Where each surface in `items` is and what screen it is on, so a menu near
/// the bottom edge flips up instead of hanging off it. The shell asks while
/// it places a popup, inside dispatch, so it is told ahead of time.
fn popup_areas(items: &[SceneItem], screens: &[Rect], bounds: Rect) -> Vec<PopupArea> {
    items
        .iter()
        .filter(|item| item.surface.is_valid())
        .map(|item| {
            let (x, y) = (item.rect.x as f64, item.rect.y as f64);
            let usable = screens
                .iter()
                .copied()
                .find(|b| b.contains(x, y))
                .unwrap_or(bounds);
            PopupArea {
                parent: item.surface,
                parent_box: item.rect,
                usable,
            }
        })
        .collect()
}

fn raise(stack: &mut Vec<SurfaceId>, surface: SurfaceId) {
    stack.retain(|&s| s != surface);
    stack.push(surface);
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut backend_options = BackendOptions {
        drm_device: std::env::args()
            .nth(1)
            .or_else(|| std::env::var("LUMINARIA_DRM_DEVICE").ok()),
        title: "minimal".into(),
        ..BackendOptions::default()
    };
    if let Ok(want) = std::env::var("LUMINARIA_BACKEND") {
        backend_options.kind = want.parse()?;
    }

    // Declaration order is destruction order, reversed, and the borrow
    // checker holds every one of these to it.
    let display = Display::new()?;
    let globals = CoreGlobals::new(&display)?;
    let (shell, seat) = (globals.shell(), globals.seat());
    let layout = OutputLayout::new()?;
    let mut popups = PopupTree::new(shell)?;

    // Nested, else DRM when a libseat session can be had, else headless; or
    // the one LUMINARIA_BACKEND names. Its outputs arrive as events once it
    // starts.
    let mut backend = AutoBackend::new(&display, &backend_options)?;
    let renderer = SceneRenderer::new(&backend.vulkan_options())?;
    renderer.set_presentation(Some(globals.presentation()));
    // GPU clients hand over dmabufs, imported with no copy.
    let _dmabuf = renderer
        .gpu_enabled()
        .then(|| LinuxDmabuf::from_scene_renderer(&display, &renderer).ok())
        .flatten();
    let mut cursor = Cursor::new_with_shapes(&renderer, &layout, seat, globals.cursor_shape())?;

    // Each output with the wl_output that follows it, dropped together.
    let mut outputs: Vec<(Output, Option<OutputGlobal>)> = Vec::new();

    let socket = display.add_socket_auto()?;
    if let Some(source) = backend.input() {
        seat.follow(source, false);
    }
    backend.start()?;
    println!("minimal: WAYLAND_DISPLAY={socket} — Logo+Return opens a terminal, Logo+Escape quits");

    let mut stack: Vec<SurfaceId> = Vec::new(); // mapped windows, bottom to top
    let mut keys = KeyRouter::new();
    let mut usable = Rect::new(0, 0, backend_options.width, backend_options.height); // the first output

    let deadline = std::env::var("LUMINARIA_EXIT_MS")
        .ok()
        .and_then(|ms| ms.parse().ok())
        .map(|ms| Instant::now() + Duration::from_millis(ms));

    'run: loop {
        let events = display.dispatch(Some(Duration::from_millis(16)))?;
        if deadline.is_some_and(|d| Instant::now() >= d) {
            break;
        }

        let mut relayout = false;
        let mut restacked = false;
        let mut redraw = false;
        let mut due = Vec::new(); // outputs that asked for a frame
        let mut actions = Vec::new();
        for event in events {
            match event {
                // --- outputs -------------------------------------------------
                Event::OutputAdded(mut output) => {
                    if let Err(e) = renderer.reset(&mut output) {
                        eprintln!("minimal: render target: {e}; compositing on the CPU");
                    }
                    layout.add_auto(&mut output);
                    let name = format!("minimal-{}", outputs.len() + 1);
                    let wl_output =
                        OutputGlobal::for_output(&display, &output, &layout, &name).ok();
                    outputs.push((output, wl_output));
                    relayout = true;
                }
                Event::OutputRemoved(id) => {
                    outputs.retain(|(o, _)| o.id() != id);
                    relayout = true;
                }
                Event::Output(id, OutputEvent::Frame) => due.push(id),
                Event::Output(_, OutputEvent::ModeChanged) => relayout = true,

                // --- windows -------------------------------------------------
                Event::Toplevel(surface, ToplevelEvent::Created) => {
                    if let Some(t) = shell.toplevel(surface) {
                        // Sized before its first frame, so it never shows at its own size.
                        t.set_maximized(true);
                        t.configure(usable.size());
                    }
                }
                Event::Toplevel(surface, ToplevelEvent::Map) => {
                    // Render at the output's density: the integer hint and, for
                    // clients that speak fractional-scale, the exact scale.
                    if let Some((first, _)) = outputs.first() {
                        globals
                            .fractional_scale()
                            .set_scale(surface, first.scale() as f64);
                    }
                    raise(&mut stack, surface);
                    restacked = true;
                }
                Event::Toplevel(surface, ToplevelEvent::Unmap | ToplevelEvent::Destroy) => {
                    stack.retain(|&s| s != surface);
                    restacked = true;
                }
                // Menus, tooltips and combo boxes: the tree follows the shell,
                // drawn above their parents and dismissed by a click outside.
                Event::Popup(..) => redraw = true,

                // --- input ---------------------------------------------------
                // One arm for either backend: they differ in which events
                // arrive (relative motion and touch from libinput, absolute
                // motion and leave from the host window), not in how.
                Event::Input(input) => match input {
                    InputEvent::Key { .. } => keys.route(seat, &input, |e| {
                        let &InputEvent::Key {
                            keysym, modifiers, ..
                        } = e
                        else {
                            return false;
                        };
                        if backend.session().is_some_and(|s| s.switch_vt_for(keysym)) {
                            seat.release_keys(); // their releases arrive on the other VT
                            return true;
                        }
                        let binding = BINDINGS
                            .iter()
                            .find(|&&(m, k, _)| m == modifiers.held() && k == keysym.to_lower());
                        actions.extend(binding.map(|&(_, _, action)| action));
                        binding.is_some()
                    }),
                    InputEvent::Modifiers(m) => seat.notify_modifiers(m),
                    InputEvent::Motion { dx, dy } => {
                        if cursor.move_by(dx, dy)
                            && let Some((x, y)) = cursor.position()
                        {
                            scene_route_pointer(seat, &scene(shell, &popups, &stack, usable), x, y);
                        }
                    }
                    InputEvent::MotionAbsolute { x, y, output } => {
                        let on = output.and_then(|id| outputs.iter().find(|(o, _)| o.id() == id));
                        let origin = on.map_or(layout.primary_box(), |(o, _)| layout.output_box(o));
                        cursor.move_to(f64::from(origin.x) + x, f64::from(origin.y) + y);
                        if let Some((x, y)) = cursor.position() {
                            scene_route_pointer(seat, &scene(shell, &popups, &stack, usable), x, y);
                        }
                    }
                    InputEvent::PointerLeave => {
                        cursor.leave();
                        seat.pointer_clear_focus();
                    }
                    InputEvent::Button { button, pressed } => {
                        let at = cursor.position();
                        if pressed {
                            popups.dismiss_grabs_outside(seat.pointer_focus()); // a click outside a menu closes it
                            if let Some((x, y)) = at {
                                let hit =
                                    scene_hit_test(&scene(shell, &popups, &stack, usable), x, y);
                                if hit.tag != 0 && stack.last() != Some(&SurfaceId(hit.tag)) {
                                    raise(&mut stack, SurfaceId(hit.tag));
                                    restacked = true;
                                }
                            }
                        }
                        if seat.pointer_focus().is_some() {
                            seat.pointer_button(button, pressed);
                        }
                        // An implicit grab may have ended; re-pick.
                        if let (false, Some((x, y))) = (pressed, at) {
                            scene_route_pointer(seat, &scene(shell, &popups, &stack, usable), x, y);
                        }
                    }
                    InputEvent::Axis(axis) => seat.pointer_axis_event(axis),
                    _ => {} // the keymap is the seat's; touch: no policy here
                },
                Event::HostCloseRequest => break 'run,
                Event::SessionActive(active) => {
                    if active {
                        redraw = true; // repaint what the other VT drew over
                    } else {
                        seat.release_keys();
                    }
                }
                _ => {}
            }
        }

        for action in actions {
            match action {
                Action::Spawn => {
                    let term = std::env::var("TERMINAL").unwrap_or_else(|_| "foot".into());
                    if let Err(e) = Command::new(&term).env("WAYLAND_DISPLAY", &socket).spawn() {
                        eprintln!("minimal: {term}: {e}");
                    }
                }
                Action::Cycle => {
                    if stack.len() > 1 {
                        stack.rotate_left(1);
                        restacked = true;
                    }
                }
                Action::Close => {
                    if let Some(t) = stack.last().and_then(|&top| shell.toplevel(top)) {
                        t.close();
                    }
                }
                Action::Quit => break 'run,
            }
        }

        // Windows fill the first output; everything else about the layout is
        // the OutputLayout's.
        if relayout && !outputs.is_empty() {
            usable = layout.primary_box();
            shell.set_bounds(usable.size());
            for t in stack.iter().filter_map(|&s| shell.toplevel(s)) {
                t.configure(usable.size());
            }
            redraw = true;
        }
        // The top window has the keyboard and the activated look, and the
        // pointer may be over something else now.
        if restacked {
            shell.focus(seat, stack.last().copied());
            if let Some((x, y)) = cursor.position() {
                scene_route_pointer(seat, &scene(shell, &popups, &stack, usable), x, y);
            }
        }
        // Nothing a client reports changed, so every output is asked for a
        // frame and the frame's diff works out what moved.
        if restacked || redraw {
            for (o, _) in &mut outputs {
                o.schedule_frame();
            }
        }

        // --- frames ----------------------------------------------------------

        let items = scene(shell, &popups, &stack, usable);
        let screens: Vec<Rect> = outputs.iter().map(|(o, _)| layout.output_box(o)).collect();
        shell.set_popup_constraint_areas(&popup_areas(&items, &screens, layout.bounds()));
        for (o, _) in outputs.iter_mut().filter(|(o, _)| due.contains(&o.id())) {
            if let Err(e) = renderer.present_in(&layout, o, &items, BACKGROUND) {
                eprintln!("minimal: present: {e}");
            }
        }
    }

    Ok(())
}
