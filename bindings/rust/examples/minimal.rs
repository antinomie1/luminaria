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
//! has the keyboard, what a key does. The pointer's picture is `Cursor`'s, and
//! answering the display — frame callbacks, the frame that turned out
//! unchanged — is `SceneRenderer`'s.
//!
//! Where it still differs from its twin, because the binding does not expose
//! it yet: no preferred buffer scale or fractional scale sent on map, and a
//! pointer grab held by a subsurface is routed as if it sat at its window's origin.

use std::process::Command;
use std::time::{Duration, Instant};

use luminaria::backend::{
    BackendInputEvent, DrmBackend, DrmEvent, HeadlessBackend, LibinputBackend, LibinputEvent,
    Output, Session, WaylandBackend,
};
use luminaria::core::{Display, Error, KeyRouter};
use luminaria::gpu::{
    Cursor, DrmDeviceId, LinuxDmabuf, SceneOutcome, SceneOutput, SceneRenderer, VulkanOptions,
};
use luminaria::protocol::{
    Compositor, CursorShapeManager, DataDeviceManager, FractionalScaleManager, OutputGlobal, PopupArea, PopupEvent,
    Presentation, Seat, SinglePixelBufferManager, Subcompositor, SurfaceId, ToplevelEvent,
    Viewporter, XdgDecorationManager, XdgEvent, XdgShell,
};
use luminaria::shell::{scene_hit_test, OutputLayout, PopupTree, SceneItem};
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

// XF86Switch_VT_1..12: what the keymap itself turns Ctrl+Alt+Fn into.
const SWITCH_VT_1: u32 = 0x1008_fe01;
const SWITCH_VT_12: u32 = 0x1008_fe0c;

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

/// `'a` is the display, plus the libseat session on DRM.
enum Backend<'a> {
    Nested(WaylandBackend<'a>),
    Drm {
        drm: DrmBackend<'a>,
        input: LibinputBackend<'a>,
    },
    Headless(HeadlessBackend<'a>),
}

impl Backend<'_> {
    fn start(&mut self) -> Result<(), Error> {
        match self {
            Backend::Nested(b) => b.start(),
            Backend::Drm { drm, .. } => drm.start(),
            Backend::Headless(b) => b.start(),
        }
    }
}

/// One output and what was built for it. Dropped when its monitor goes away.
/// `'a` is the renderer `state` caches into and the display `_global` is on.
struct Screen<'a> {
    output: Output,
    state: SceneOutput<'a>,
    global: Option<OutputGlobal<'a>>,
}

/// What the frames did, once a second on DRM and only when it changed: the
/// line scripts/tty-check.sh reads from the C++ twin. An idle screen asks for
/// no frames at all, and then this line stops appearing — which is the point.
#[derive(Default, Clone, Copy, PartialEq)]
struct FrameCount {
    composited: u32,
    scanout: u32,
    unchanged: u32,
    cpu: u32,
}

/// Lays a new output out to the right of the others and gives it a render
/// target and a wl_output.
fn add_screen<'a>(
    display: &'a Display,
    renderer: &'a SceneRenderer,
    layout: &OutputLayout,
    screens: &mut Vec<Screen<'a>>,
    mut output: Output,
) {
    layout.add_auto(&mut output);
    let mut state = SceneOutput::new();
    if let Err(e) = renderer.reset(&mut state, &mut output, DRM_FORMAT_XRGB8888) {
        eprintln!("minimal: render target: {e}; compositing on the CPU");
    }
    let b = layout.output_box(&output);
    let name = format!("minimal-{}", screens.len() + 1);
    let global = OutputGlobal::new(display, output.width(), output.height(), &name).ok();
    if let Some(g) = &global {
        g.set_logical_position(b.x, b.y);
        g.set_mode(output.width(), output.height(), output.current_mode().refresh_mhz);
        g.set_scale(output.scale());
        g.set_transform(output.transform());
    }
    println!(
        "minimal: output {}x{}@{:.3}Hz at ({},{}), scale {}, cursor plane {}",
        output.width(),
        output.height(),
        output.current_mode().refresh_mhz as f64 / 1000.0,
        b.x,
        b.y,
        output.scale(),
        if output.has_cursor_plane() { "yes" } else { "no" }
    );
    screens.push(Screen { output, state, global });
}

/// Every window sits at the usable area's origin, newest on top, with its
/// popups above it. Tagged with its own surface, so a hit on one of its
/// subsurfaces or popups still names the window.
fn scene(shell: &XdgShell, popups: &PopupTree, stack: &[SurfaceId], usable: Rect) -> Vec<SceneItem> {
    let mut items: Vec<SceneItem> = stack
        .iter()
        .filter_map(|&s| {
            // The client's shadow sits outside its window geometry, so the
            // geometry, not the buffer, is what lands on the usable area.
            let g = shell.toplevel(s)?.geometry();
            let framed = g.width > 0 && g.height > 0;
            let (w, h) = if framed { (g.width, g.height) } else { (s.surface_width(), s.surface_height()) };
            let (ox, oy) = if framed { (g.x, g.y) } else { (0, 0) };
            Some(SceneItem::surface(s, Rect::new(usable.x, usable.y, w, h), usable.x - ox, usable.y - oy).with_tag(s.0))
        })
        .collect();
    popups.append_to(&mut items);
    items
}

/// Where each surface in `items` is and what screen it is on, so a menu near
/// the bottom edge flips up instead of hanging off it. The shell asks while
/// it places a popup, inside dispatch, so it is told ahead of time.
fn popup_areas(items: &[SceneItem], layout: &OutputLayout, screens: &[Screen]) -> Vec<PopupArea> {
    items
        .iter()
        .filter(|item| item.surface.is_valid())
        .map(|item| {
            let (x, y) = (item.rect.x as f64, item.rect.y as f64);
            let usable = screens
                .iter()
                .map(|s| layout.output_box(&s.output))
                .find(|b| b.contains(x, y))
                .unwrap_or_else(|| layout.bounds());
            PopupArea { parent: item.surface, parent_box: item.rect, usable }
        })
        .collect()
}

/// `items` (layout coordinates) in the coordinates of the output at `view`.
fn local(items: &[SceneItem], view: Rect) -> Vec<SceneItem> {
    items
        .iter()
        .map(|&item| {
            let mut item = item;
            item.rect.x -= view.x;
            item.rect.y -= view.y;
            item.surface_origin.0 -= view.x;
            item.surface_origin.1 -= view.y;
            item
        })
        .collect()
}

/// The top window has the keyboard; every other one is told it lost it.
fn focus_top(stack: &[SurfaceId], shell: &XdgShell, seat: &Seat) {
    for (i, &s) in stack.iter().enumerate() {
        if let Some(t) = shell.toplevel(s) {
            t.set_activated(i + 1 == stack.len());
        }
    }
    seat.set_keyboard_focus(stack.last().copied());
}

fn raise(stack: &mut Vec<SurfaceId>, surface: SurfaceId) {
    stack.retain(|&s| s != surface);
    stack.push(surface);
}

/// Gives the pointer to whatever is under it. A held button keeps it on the
/// surface it went down on, in that surface's coordinates.
fn pointer_focus(seat: &Seat, items: &[SceneItem], usable: Rect, (x, y): (f64, f64)) {
    if let Some(grab) = seat.pointer_grab() {
        let (ox, oy) = items
            .iter()
            .find(|item| item.surface == grab)
            .map_or((usable.x, usable.y), |item| item.surface_origin);
        seat.pointer_motion(x - ox as f64, y - oy as f64);
        return;
    }
    let hit = scene_hit_test(items, x, y);
    match hit.surface {
        Some(surface) => seat.pointer_enter(surface, hit.sx, hit.sy),
        None if seat.pointer_focus().is_some() => seat.pointer_clear_focus(),
        None => {}
    }
}

/// The windows and popups a button lands among.
struct Desk<'s, 'd> {
    shell: &'s XdgShell<'d>,
    popups: &'s mut PopupTree,
    stack: &'s mut Vec<SurfaceId>,
    usable: Rect,
}

/// A button: a press outside an open menu closes it, and raises the window
/// under the pointer. Returns whether the stack changed.
fn button(seat: &Seat, desk: Desk, at: Option<(f64, f64)>, code: u32, pressed: bool) -> bool {
    let mut restacked = false;
    if pressed {
        desk.popups.dismiss_grabs_outside(seat.pointer_focus()); // menu semantics
    }
    if let (true, Some((x, y))) = (pressed, at) {
        let hit = scene_hit_test(&scene(desk.shell, desk.popups, desk.stack, desk.usable), x, y);
        if hit.tag != 0 && desk.stack.last() != Some(&SurfaceId(hit.tag)) {
            raise(desk.stack, SurfaceId(hit.tag));
            restacked = true;
        }
    }
    if seat.pointer_focus().is_some() {
        seat.pointer_button(code, pressed);
    }
    if let (false, Some(at)) = (pressed, at) {
        // An implicit grab may have ended.
        pointer_focus(seat, &scene(desk.shell, desk.popups, desk.stack, desk.usable), desk.usable, at);
    }
    restacked
}

/// What a key asked the compositor to do, rather than a client.
enum KeyCommand {
    Run(Action),
    SwitchVt(i32),
}

/// A key: a binding, a VT switch, or the focused client's. `state` is the
/// keysym and modifier mask it is matched against, when there is a keymap.
fn key(
    seat: &Seat,
    keys: &mut KeyRouter,
    held: &mut Vec<u32>,
    code: u32,
    pressed: bool,
    state: Option<(u32, u32)>,
    vt_switching: bool,
) -> Option<KeyCommand> {
    if !pressed {
        if keys.unconsume(code) {
            return None; // the release of a key we kept stays ours
        }
        held.retain(|&k| k != code);
        seat.notify_key(code, false);
        return None;
    }
    // A press of a key we think we still hold: its release was lost, which is
    // what happens to the key that switched VTs away.
    keys.unconsume(code);
    if let Some((sym, mods)) = state {
        if vt_switching && (SWITCH_VT_1..=SWITCH_VT_12).contains(&sym) {
            keys.consume(code);
            return Some(KeyCommand::SwitchVt((sym - SWITCH_VT_1 + 1) as i32));
        }
        let mods = mods & BINDABLE_MODS;
        if let Some(&(_, _, action)) = BINDINGS.iter().find(|&&(m, s, _)| m == mods && s == sym) {
            keys.consume(code);
            return Some(KeyCommand::Run(action));
        }
    }
    held.push(code);
    seat.notify_key(code, true);
    None
}

/// Every key a client was told is down goes up: the VT is being taken away and
/// the releases will never arrive.
fn release_held(seat: &Seat, held: &mut Vec<u32>) {
    for k in held.drain(..) {
        seat.notify_key(k, false);
    }
    seat.notify_modifiers(0, 0, 0, 0);
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let device = std::env::args()
        .nth(1)
        .or_else(|| std::env::var("LUMINARIA_DRM_DEVICE").ok());

    // Declaration order is destruction order, reversed. The renderer first, so
    // it dies last: every output state caches textures it owns, which is why
    // `SceneOutput` borrows it. It is created once the card is open, so it can
    // be the GPU that card belongs to. Everything after the display borrows it.
    let renderer;
    let display = Display::new()?;
    display.init_shm()?;
    // Before the backend: DRM and libinput keep the session borrowed.
    let mut session: Option<Session> = None;

    let _compositor = Compositor::new(&display)?;
    let _subcompositor = Subcompositor::new(&display)?;
    let shell = XdgShell::new(&display)?;
    let seat = Seat::new(&display, "seat0")?;
    let _data_device = DataDeviceManager::new(&display, &seat)?;
    let _single_pixel = SinglePixelBufferManager::new(&display)?;
    let presentation = Presentation::new(&display)?;
    let _viewporter = Viewporter::new(&display)?;
    let _fractional = FractionalScaleManager::new(&display)?;
    // We draw no title bars, so clients draw their own.
    let _decoration = XdgDecorationManager::new(&display)?;
    let cursor_shape = CursorShapeManager::new(&display)?;
    let layout = OutputLayout::new()?;
    let mut popups = PopupTree::new()?;

    // Backends after the globals: an output's last `present` must still find
    // the presentation global there.
    //
    // Picks the backend. Nested and headless come with their one output; DRM's
    // arrive as `DrmEvent`s once it starts. Also answers which GPU the display
    // belongs to, so the renderer can be that one. Inline rather than a
    // function: a DRM backend that fails gives the session back, and only
    // here can the borrow checker see that its borrow of the session ended.
    let want = std::env::var("LUMINARIA_BACKEND").unwrap_or_else(|_| "auto".into());
    let (mut backend, outputs, gpu_options) = 'pick: {
        if want == "auto" || want == "nested" {
            match WaylandBackend::new(&display) {
                Ok(mut nested) => {
                    let output = nested.add_output(WIDTH, HEIGHT)?;
                    break 'pick (Backend::Nested(nested), vec![output], VulkanOptions::default());
                }
                Err(e) if want == "nested" => return Err(e.into()),
                Err(_) => {}
            }
        }
        if want == "auto" || want == "drm" {
            // With a seat a VT switch is safe: DRM master is handed back and
            // input suspended. Auto only tries DRM with one, so ssh and CI never
            // land on a VT by accident; asked for explicitly, it also runs without.
            match Session::new(&display) {
                Ok(s) => {
                    session = Some(s);
                    println!("minimal: session = libseat (VT switching enabled)");
                }
                Err(e) if want == "drm" => {
                    println!("minimal: session = none ({e}); VT switching disabled");
                }
                Err(_) => {}
            }
            if want == "drm" || session.is_some() {
                match DrmBackend::new(&display, device.as_deref(), session.as_ref()) {
                    Ok(drm) => {
                        let options = VulkanOptions {
                            drm_device: DrmDeviceId::from_fd(&drm),
                        };
                        let input = LibinputBackend::new(&display, session.as_ref())?;
                        break 'pick (Backend::Drm { drm, input }, Vec::new(), options);
                    }
                    Err(e) if want == "drm" => return Err(e.into()),
                    Err(_) => {}
                }
                session = None;
            }
        }
        let mut headless = HeadlessBackend::new(&display, 16)?;
        let output = headless.add_output(WIDTH, HEIGHT)?;
        (Backend::Headless(headless), vec![output], VulkanOptions::default())
    };
    println!(
        "minimal: backend = {}",
        match backend {
            Backend::Nested(_) => "nested",
            Backend::Drm { .. } => "drm",
            Backend::Headless(_) => "headless",
        }
    );

    renderer = SceneRenderer::new(&gpu_options)?;
    if renderer.gpu_enabled() {
        println!("minimal: renderer = GPU");
    } else {
        println!("minimal: renderer = CPU ({})", renderer.gpu_error());
    }
    renderer.set_presentation(Some(&presentation));
    let _dmabuf = if renderer.gpu_enabled() {
        LinuxDmabuf::from_scene_renderer(&display, &renderer).ok()
    } else {
        None
    };
    let mut cursor = Cursor::new_with_shapes(&renderer, &layout, &seat, &cursor_shape)?;

    let mut screens: Vec<Screen> = Vec::new();
    for output in outputs {
        add_screen(&display, &renderer, &layout, &mut screens, output);
    }

    // Nested: the host's layout, matched against here and sent to clients.
    // DRM: libinput's, which its modifier masks are computed against.
    let mut keymap: Option<KeymapState> = None;
    let socket = display.add_socket_auto()?;
    match &mut backend {
        Backend::Drm { input, .. } => {
            seat.set_keymap(&input.keymap());
            input.start()?;
            let caps = input.capabilities();
            seat.set_capabilities(caps.keyboard, caps.pointer, false);
        }
        _ => seat.set_capabilities(true, true, false),
    }
    backend.start()?;
    if let Backend::Nested(nested) = &backend {
        let text = nested.keymap();
        if let Ok(k) = KeymapState::from_text(&text) {
            if seat.set_keymap(&text) {
                keymap = Some(k);
            }
        }
    }
    let on_drm = matches!(backend, Backend::Drm { .. });
    println!("minimal: WAYLAND_DISPLAY={socket} — Logo+Return opens a terminal, Logo+Escape quits");

    let mut stack: Vec<SurfaceId> = Vec::new(); // bottom to top
    let mut keys = KeyRouter::new();
    let mut held: Vec<u32> = Vec::new(); // keys a client was told are down
    let mut usable = Rect::new(0, 0, WIDTH, HEIGHT); // the first output: where windows go
    let mut relayout = true;
    let (mut frames, mut reported) = (FrameCount::default(), FrameCount::default());
    let mut next_report = Instant::now() + Duration::from_secs(1);

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

        // --- outputs ---------------------------------------------------------

        if let Backend::Drm { drm, .. } = &mut backend {
            while let Some(event) = drm.next_event() {
                match event {
                    DrmEvent::OutputAdded(output) => {
                        add_screen(&display, &renderer, &layout, &mut screens, output);
                        // libinput only reports deltas: the pointer starts mid-screen.
                        if cursor.position().is_none() {
                            let b = layout.bounds();
                            let (x, y) = (b.x as f64 + b.width as f64 / 2.0, b.y as f64 + b.height as f64 / 2.0);
                            cursor.move_to(x, y, b);
                        }
                    }
                    DrmEvent::OutputRemoved(id) => {
                        // The layout has already let go of it.
                        println!("minimal: output removed");
                        screens.retain(|s| s.output.id() != id);
                    }
                }
                relayout = true;
            }
        }
        // Everything sized for the old mode is rebuilt.
        for s in &mut screens {
            if s.output.take_mode_changed() {
                let _ = renderer.reset(&mut s.state, &mut s.output, DRM_FORMAT_XRGB8888);
                let old = layout.output_box(&s.output);
                layout.add(&mut s.output, old.x, old.y);
                if let Some(g) = &s.global {
                    g.set_mode(s.output.width(), s.output.height(), s.output.current_mode().refresh_mhz);
                }
                relayout = true;
            }
        }
        // Windows fill the first output; everything else about the layout is
        // the OutputLayout's.
        if relayout {
            relayout = false;
            if let Some(first) = screens.first() {
                usable = layout.output_box(&first.output);
                shell.set_bounds(usable.width, usable.height);
                for &s in &stack {
                    if let Some(t) = shell.toplevel(s) {
                        t.configure(usable.width, usable.height);
                    }
                }
                cursor.refresh();
                for s in &mut screens {
                    s.output.schedule_frame();
                }
            }
        }

        // --- windows ---------------------------------------------------------

        let mut restacked = false;
        let mut redraw = false;
        while let Some(event) = shell.next_event() {
            match event {
                XdgEvent::Toplevel(surface, event) => {
                    let Some(t) = shell.toplevel(surface) else {
                        // Destroyed since: only its leaving matters.
                        stack.retain(|&s| s != surface);
                        restacked = true;
                        continue;
                    };
                    match event {
                        ToplevelEvent::Created => {
                            // Sized before its first frame, so it never shows at its own size.
                            t.set_maximized(true);
                            t.configure(usable.width, usable.height);
                        }
                        ToplevelEvent::Map => {
                            println!("minimal: window mapped — \"{}\"", t.title());
                            raise(&mut stack, surface);
                            restacked = true;
                        }
                        ToplevelEvent::Unmap | ToplevelEvent::Destroy => {
                            stack.retain(|&s| s != surface);
                            restacked = true;
                        }
                        // The twin listens to none of these, and luminaria then
                        // grants them itself. The binding always listens, so
                        // the same answer is given here.
                        ToplevelEvent::RequestMaximize(on) => t.set_maximized(on),
                        ToplevelEvent::RequestFullscreen(on) => t.set_fullscreen(on),
                        ToplevelEvent::RequestMinimize => t.set_minimized(true),
                        _ => {}
                    }
                }
                // Menus, tooltips and combo boxes: drawn above their parents,
                // dismissed by a click outside them. A destroyed one has
                // already left the tree.
                XdgEvent::Popup(popup, event) => {
                    match event {
                        PopupEvent::Created { .. } => {
                            popups.add(&shell, popup);
                        }
                        PopupEvent::Map => popups.set_mapped(popup, true),
                        PopupEvent::Unmap => popups.set_mapped(popup, false),
                        _ => {}
                    }
                    redraw = true;
                }
            }
        }

        // --- input -----------------------------------------------------------

        let mut commands: Vec<KeyCommand> = Vec::new();
        match &mut backend {
            Backend::Nested(nested) => {
                while let Some(event) = nested.next_input_event() {
                    match event {
                        BackendInputEvent::Motion { x, .. } if x < 0.0 => {
                            // Left the host window.
                            cursor.leave();
                            seat.pointer_clear_focus();
                        }
                        BackendInputEvent::Motion { x, y } => {
                            cursor.move_to(usable.x as f64 + x, usable.y as f64 + y, layout.bounds());
                            if let Some(at) = cursor.position() {
                                pointer_focus(&seat, &scene(&shell, &popups, &stack, usable), usable, at);
                            }
                        }
                        BackendInputEvent::Button { button: code, pressed } => {
                            let at = cursor.position();
                            let desk = Desk { shell: &shell, popups: &mut popups, stack: &mut stack, usable };
                            restacked |= button(&seat, desk, at, code, pressed);
                        }
                        BackendInputEvent::Key { key: code, pressed } => {
                            // The host sends no modifier events through the
                            // binding, so the masks come from our own copy of its
                            // keymap.
                            let state = keymap.as_mut().map(|k| {
                                k.update_key(code, pressed);
                                let (depressed, latched, locked, group) = k.modifiers();
                                seat.notify_modifiers(depressed, latched, locked, group);
                                (k.keysym(code), depressed | latched)
                            });
                            commands.extend(key(&seat, &mut keys, &mut held, code, pressed, state, false));
                        }
                        BackendInputEvent::Modifiers(mods) => {
                            seat.notify_modifiers(mods.depressed, mods.latched, mods.locked, mods.group);
                        }
                        BackendInputEvent::Axis {
                            dx,
                            dy,
                            dx_steps,
                            dy_steps,
                            stop_horizontal,
                            stop_vertical,
                        } => {
                            if dx != 0.0 || dy != 0.0 {
                                seat.pointer_axis(dx, dy);
                            }
                            if dx_steps != 0 || dy_steps != 0 {
                                seat.pointer_axis_discrete(dx_steps, dy_steps);
                            }
                            if stop_horizontal || stop_vertical {
                                seat.pointer_axis_stop(stop_horizontal, stop_vertical);
                            }
                        }
                        BackendInputEvent::KeymapChange(text) => {
                            seat.set_keymap(&text);
                            keymap = KeymapState::from_text(&text).ok();
                        }
                        BackendInputEvent::HostCloseRequest => {
                            break 'run;
                        }
                    }
                }
            }
            Backend::Drm { input, .. } => {
                while let Some(event) = input.next_input_event() {
                    match event {
                        LibinputEvent::Motion { dx, dy } => {
                            if cursor.move_by(dx, dy, layout.bounds()) {
                                if let Some(at) = cursor.position() {
                                    pointer_focus(&seat, &scene(&shell, &popups, &stack, usable), usable, at);
                                }
                            }
                        }
                        LibinputEvent::Button { button: code, pressed } => {
                            let at = cursor.position();
                            let desk = Desk { shell: &shell, popups: &mut popups, stack: &mut stack, usable };
                            restacked |= button(&seat, desk, at, code, pressed);
                        }
                        LibinputEvent::Axis {
                            dx,
                            dy,
                            dx_steps,
                            dy_steps,
                            stop_horizontal,
                            stop_vertical,
                        } => {
                            if dx_steps != 0 || dy_steps != 0 {
                                seat.pointer_axis_discrete(dx_steps, dy_steps);
                            } else if dx != 0.0 || dy != 0.0 {
                                seat.pointer_axis(dx, dy);
                            }
                            if stop_horizontal || stop_vertical {
                                seat.pointer_axis_stop(stop_horizontal, stop_vertical);
                            }
                        }
                        LibinputEvent::Key {
                            key: code,
                            pressed,
                            keysym,
                            modifiers,
                        } => {
                            let state = Some((keysym, modifiers.depressed | modifiers.latched));
                            let vt = session.is_some();
                            commands.extend(key(&seat, &mut keys, &mut held, code, pressed, state, vt));
                        }
                        LibinputEvent::Modifiers(m) => {
                            seat.notify_modifiers(m.depressed, m.latched, m.locked, m.group);
                        }
                        // A client that binds a wl_pointer no device can drive waits forever.
                        LibinputEvent::Capabilities(caps) => {
                            seat.set_capabilities(caps.keyboard, caps.pointer, false);
                            println!(
                                "minimal: input devices: keyboard={} pointer={}",
                                if caps.keyboard { "yes" } else { "no" },
                                if caps.pointer { "yes" } else { "no" }
                            );
                        }
                        // No touch policy here, as in the twin.
                        _ => {}
                    }
                }
            }
            Backend::Headless(_) => {}
        }
        for command in commands {
            match command {
                KeyCommand::SwitchVt(vt) => {
                    release_held(&seat, &mut held);
                    if let Some(s) = &session {
                        let _ = s.switch_to(vt);
                    }
                }
                KeyCommand::Run(Action::Spawn) => {
                    let term = std::env::var("TERMINAL").unwrap_or_else(|_| "foot".into());
                    if let Err(e) = Command::new(&term).env("WAYLAND_DISPLAY", &socket).spawn() {
                        eprintln!("minimal: {term}: {e}");
                    }
                }
                KeyCommand::Run(Action::Cycle) => {
                    if stack.len() > 1 {
                        stack.rotate_left(1);
                        restacked = true;
                    }
                }
                KeyCommand::Run(Action::Close) => {
                    if let Some(t) = stack.last().and_then(|&top| shell.toplevel(top)) {
                        t.close();
                    }
                }
                KeyCommand::Run(Action::Quit) => break 'run,
            }
        }

        if let Some(sess) = &session {
            while let Some(active) = sess.next_activity() {
                if active {
                    println!("minimal: session active — VT switched in");
                    for s in &mut screens {
                        s.output.schedule_frame(); // repaint what the other VT drew over
                    }
                } else {
                    println!("minimal: session inactive — VT switched away");
                    release_held(&seat, &mut held);
                }
            }
        }

        // Nothing a client reports changed, so every output is asked for a
        // frame and the frame's diff works out what moved.
        if restacked {
            focus_top(&stack, &shell, &seat);
            if let Some(at) = cursor.position() {
                pointer_focus(&seat, &scene(&shell, &popups, &stack, usable), usable, at);
            }
        }
        if restacked || redraw {
            for s in &mut screens {
                s.output.schedule_frame();
            }
        }

        // --- frames ----------------------------------------------------------

        let items = scene(&shell, &popups, &stack, usable);
        shell.set_popup_constraint_areas(&popup_areas(&items, &layout, &screens));
        for s in &mut screens {
            if !s.output.take_frame() {
                continue;
            }
            let view = layout.output_box(&s.output);
            match renderer.present(&mut s.state, &mut s.output, &local(&items, view), BACKGROUND) {
                Ok(SceneOutcome::Composited) => frames.composited += 1,
                Ok(SceneOutcome::Scanout) => frames.scanout += 1,
                Ok(SceneOutcome::Unchanged) => frames.unchanged += 1,
                Ok(SceneOutcome::Cpu) => frames.cpu += 1,
                Err(e) => eprintln!("minimal: present: {e}"),
            }
        }
        if on_drm && Instant::now() >= next_report {
            next_report += Duration::from_secs(1);
            if frames != reported {
                println!(
                    "minimal: frames/s composited={} scanout={} unchanged={} cpu={}",
                    frames.composited - reported.composited,
                    frames.scanout - reported.scanout,
                    frames.unchanged - reported.unchanged,
                    frames.cpu - reported.cpu
                );
                reported = frames;
            }
        }
    }

    Ok(())
}
