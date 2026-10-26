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
//! it yet: no popups (`PopupTree` has no way in), no `LinuxDmabuf` on the
//! renderer's GPU (`SceneRenderer` does not hand out its `VulkanRenderer`), no
//! cursor-shape manager for `Cursor`, and the `OutputGlobal`s are not told an
//! output's position, refresh, scale or transform.

use std::process::Command;
use std::time::{Duration, Instant};

use luminaria::backend::{
    BackendInputEvent, DrmBackend, DrmEvent, HeadlessBackend, LibinputBackend, LibinputEvent,
    Output, Session, WaylandBackend,
};
use luminaria::core::{Display, Error, KeyRouter};
use luminaria::gpu::{Cursor, DrmDeviceId, SceneOutcome, SceneOutput, SceneRenderer, VulkanOptions};
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

enum Backend {
    Nested(WaylandBackend),
    Drm {
        drm: DrmBackend,
        input: LibinputBackend,
    },
    Headless(HeadlessBackend),
}

impl Backend {
    fn start(&mut self) -> Result<(), Error> {
        match self {
            Backend::Nested(b) => b.start(),
            Backend::Drm { drm, .. } => drm.start(),
            Backend::Headless(b) => b.start(),
        }
    }
}

/// One output and what was built for it. Dropped when its monitor goes away.
struct Screen {
    output: Output,
    state: SceneOutput,
    _global: Option<OutputGlobal>,
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

/// Picks the backend. Nested and headless come with their one output; DRM's
/// arrive as `DrmEvent`s once it starts. Also answers which GPU the display
/// belongs to, so the renderer can be that one.
fn open_backend(
    display: &Display,
    session: &mut Option<Session>,
    device: Option<&str>,
) -> Result<(Backend, Vec<Output>, VulkanOptions), Error> {
    let want = std::env::var("LUMINARIA_BACKEND").unwrap_or_else(|_| "auto".into());
    if want == "auto" || want == "nested" {
        match WaylandBackend::new(display) {
            Ok(mut nested) => {
                let output = nested.add_output(WIDTH, HEIGHT)?;
                return Ok((Backend::Nested(nested), vec![output], VulkanOptions::default()));
            }
            Err(e) if want == "nested" => return Err(e),
            Err(_) => {}
        }
    }
    if want == "auto" || want == "drm" {
        // With a seat a VT switch is safe: DRM master is handed back and input
        // suspended. Auto only tries DRM with one, so ssh and CI never land on
        // a VT by accident; asked for explicitly, it also runs without.
        match Session::new(display) {
            Ok(s) => {
                *session = Some(s);
                println!("minimal: session = libseat (VT switching enabled)");
            }
            Err(e) if want == "drm" => {
                println!("minimal: session = none ({e}); VT switching disabled");
            }
            Err(_) => {}
        }
        if want == "drm" || session.is_some() {
            match DrmBackend::new(display, device, session.as_ref()) {
                Ok(drm) => {
                    let options = VulkanOptions {
                        drm_device: DrmDeviceId::from_fd(&drm),
                    };
                    let input = LibinputBackend::new(display, session.as_ref())?;
                    return Ok((Backend::Drm { drm, input }, Vec::new(), options));
                }
                Err(e) if want == "drm" => return Err(e),
                Err(_) => *session = None,
            }
        }
    }
    let mut headless = HeadlessBackend::new(display, 16)?;
    let output = headless.add_output(WIDTH, HEIGHT)?;
    Ok((Backend::Headless(headless), vec![output], VulkanOptions::default()))
}

/// Lays a new output out to the right of the others and gives it a render
/// target and a wl_output.
fn add_screen(
    display: &Display,
    renderer: &mut SceneRenderer,
    layout: &mut OutputLayout,
    screens: &mut Vec<Screen>,
    mut output: Output,
) {
    layout.add_auto(&mut output);
    let mut state = SceneOutput::new();
    if let Err(e) = renderer.reset(&mut state, &mut output, DRM_FORMAT_XRGB8888) {
        eprintln!("minimal: render target: {e}; compositing on the CPU");
    }
    let name = format!("minimal-{}", screens.len() + 1);
    let global = OutputGlobal::new(display, output.width(), output.height(), &name).ok();
    let b = layout.output_box(&output);
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
    screens.push(Screen {
        output,
        state,
        _global: global,
    });
}

/// Every window covers the whole usable area. Tagged with its own surface, so
/// a hit on one of its subsurfaces still names the window.
fn scene(stack: &[SurfaceId], usable: Rect) -> Vec<SceneItem> {
    stack
        .iter()
        .map(|&s| SceneItem::surface(s, usable, usable.x, usable.y).with_tag(s.0))
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
/// surface it went down on; every window sits at the usable area's origin.
fn pointer_focus(seat: &mut Seat, stack: &[SurfaceId], usable: Rect, (x, y): (f64, f64)) {
    if seat.pointer_grab().is_some() {
        seat.pointer_motion(x - usable.x as f64, y - usable.y as f64);
        return;
    }
    let hit = scene_hit_test(&scene(stack, usable), x, y);
    match hit.surface {
        Some(surface) => seat.pointer_enter(surface, hit.sx, hit.sy),
        None if seat.pointer_focus().is_some() => seat.pointer_clear_focus(),
        None => {}
    }
}

/// A button: a press raises the window under the pointer. Returns whether the
/// stack changed.
fn button(
    seat: &mut Seat,
    stack: &mut Vec<SurfaceId>,
    usable: Rect,
    at: Option<(f64, f64)>,
    code: u32,
    pressed: bool,
) -> bool {
    let mut restacked = false;
    if let (true, Some((x, y))) = (pressed, at) {
        let hit = scene_hit_test(&scene(stack, usable), x, y);
        if hit.tag != 0 && stack.last() != Some(&SurfaceId(hit.tag)) {
            raise(stack, SurfaceId(hit.tag));
            restacked = true;
        }
    }
    if seat.pointer_focus().is_some() {
        seat.pointer_button(code, pressed);
    }
    if let (false, Some(at)) = (pressed, at) {
        pointer_focus(seat, stack, usable, at); // an implicit grab may have ended
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
    seat: &mut Seat,
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
fn release_held(seat: &mut Seat, held: &mut Vec<u32>) {
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
    // it dies last: every output state caches textures it owns. It is created
    // once the card is open, so it can be the GPU that card belongs to.
    let mut renderer;
    let display = Display::new()?;
    display.init_shm()?;
    let mut session: Option<Session> = None;

    let _compositor = Compositor::new(&display)?;
    let _subcompositor = Subcompositor::new(&display)?;
    let mut shell = XdgShell::new(&display)?;
    let mut seat = Seat::new(&display, "seat0")?;
    let _data_device = DataDeviceManager::new(&display, &seat)?;
    let _single_pixel = SinglePixelBufferManager::new(&display)?;
    let presentation = Presentation::new(&display)?;
    let _viewporter = Viewporter::new(&display)?;
    let _fractional = FractionalScaleManager::new(&display)?;
    // We draw no title bars, so clients draw their own.
    let _decoration = XdgDecorationManager::new(&display)?;
    let mut layout = OutputLayout::new()?;

    // Backends after the globals: an output's last `present` must still find
    // the presentation global there.
    let (mut backend, outputs, gpu_options) = open_backend(&display, &mut session, device.as_deref())?;
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
    let mut cursor = Cursor::new(&mut renderer, &layout, &mut seat)?;

    let mut screens: Vec<Screen> = Vec::new();
    for output in outputs {
        add_screen(&display, &mut renderer, &mut layout, &mut screens, output);
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
                        add_screen(&display, &mut renderer, &mut layout, &mut screens, output);
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
                    shell.configure(s, usable.width, usable.height);
                }
                cursor.refresh();
                for s in &mut screens {
                    s.output.schedule_frame();
                }
            }
        }

        // --- windows ---------------------------------------------------------

        let mut restacked = false;
        while let Some(event) = shell.next_event() {
            match event {
                ToplevelEvent::Created(surface) => {
                    // Sized before its first frame, so it never shows at its own size.
                    shell.set_maximized(surface, true);
                    shell.configure(surface, usable.width, usable.height);
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
                                pointer_focus(&mut seat, &stack, usable, at);
                            }
                        }
                        BackendInputEvent::Button { button: code, pressed } => {
                            let at = cursor.position();
                            restacked |= button(&mut seat, &mut stack, usable, at, code, pressed);
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
                            commands.extend(key(&mut seat, &mut keys, &mut held, code, pressed, state, false));
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
                                    pointer_focus(&mut seat, &stack, usable, at);
                                }
                            }
                        }
                        LibinputEvent::Button { button: code, pressed } => {
                            let at = cursor.position();
                            restacked |= button(&mut seat, &mut stack, usable, at, code, pressed);
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
                            commands.extend(key(&mut seat, &mut keys, &mut held, code, pressed, state, vt));
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
                    release_held(&mut seat, &mut held);
                    if let Some(s) = session.as_mut() {
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
                    if let Some(&top) = stack.last() {
                        shell.close(top);
                    }
                }
                KeyCommand::Run(Action::Quit) => break 'run,
            }
        }

        if let Some(sess) = session.as_mut() {
            while let Some(active) = sess.next_activity() {
                if active {
                    println!("minimal: session active — VT switched in");
                    for s in &mut screens {
                        s.output.schedule_frame(); // repaint what the other VT drew over
                    }
                } else {
                    println!("minimal: session inactive — VT switched away");
                    release_held(&mut seat, &mut held);
                }
            }
        }

        // Nothing a client reports changed, so every output is asked for a
        // frame and the frame's diff works out what moved.
        if restacked {
            focus_top(&stack, &mut shell, &mut seat);
            if let Some(at) = cursor.position() {
                pointer_focus(&mut seat, &stack, usable, at);
            }
            for s in &mut screens {
                s.output.schedule_frame();
            }
        }

        // --- frames ----------------------------------------------------------

        let items = scene(&stack, usable);
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
