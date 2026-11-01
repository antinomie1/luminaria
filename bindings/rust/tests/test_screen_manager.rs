//! `ScreenManager` and `InputRouter`, the twins of tests/test_screen_manager.cpp
//! and tests/test_input_router.cpp: a real client on a real window, and nothing on
//! the Rust side that connects a frame, a present, a popup query or a seat by
//! hand. C++ never calls Rust, so the scene is told ahead of time and what a key
//! or a click means arrives as an event.

#[macro_use]
mod common;

use common::{ClientSide, Peer, shm_buffer, take_all};
use luminaria::Event;
use luminaria::backend::{AutoBackend, BackendKind, BackendOptions, InputEvent, Modifiers};
use luminaria::core::Display;
use luminaria::gpu::{Cursor, SceneRenderer};
use luminaria::protocol::{CoreGlobals, SurfaceId, ToplevelEvent};
use luminaria::shell::{
    InputRouter, OutputLayout, PopupTree, SceneItem, ScreenManager, ScreenOptions,
};
use luminaria::util::{Color, Keysym, Rect};
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::{
    wl_buffer, wl_callback, wl_compositor, wl_keyboard, wl_output, wl_pointer, wl_seat, wl_shm,
    wl_shm_pool, wl_surface,
};
use wayland_client::{Connection, Dispatch, QueueHandle, WEnum};
use wayland_protocols::xdg::shell::client::{
    xdg_popup, xdg_positioner, xdg_surface, xdg_toplevel, xdg_wm_base,
};

const WIDTH: i32 = 64;
const HEIGHT: i32 = 48;
const BTN_LEFT: u32 = 0x110;
const KEY_A: u32 = 30;
const KEY_Q: u32 = 16;
const QUIT: u32 = 9;

#[derive(Default)]
struct State {
    output_name: Option<String>,
    frame_done: bool,
    popup_y: Option<i32>,
    pointer_at: Option<(f64, f64)>,
    pointer_inside: bool,
    keyboard_inside: bool,
    buttons: Vec<(u32, bool)>,
    keys: Vec<u32>,
}

registry_state!(State);
ignore_events!(State:
    wl_compositor::WlCompositor,
    wl_surface::WlSurface,
    wl_shm::WlShm,
    wl_shm_pool::WlShmPool,
    wl_buffer::WlBuffer,
    wl_seat::WlSeat,
    xdg_positioner::XdgPositioner,
    xdg_toplevel::XdgToplevel,
);

impl Dispatch<xdg_wm_base::XdgWmBase, ()> for State {
    fn event(
        _: &mut Self,
        wm: &xdg_wm_base::XdgWmBase,
        event: xdg_wm_base::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_wm_base::Event::Ping { serial } = event {
            wm.pong(serial);
        }
    }
}

impl Dispatch<xdg_surface::XdgSurface, ()> for State {
    fn event(
        _: &mut Self,
        xs: &xdg_surface::XdgSurface,
        event: xdg_surface::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_surface::Event::Configure { serial } = event {
            xs.ack_configure(serial);
        }
    }
}

impl Dispatch<xdg_popup::XdgPopup, ()> for State {
    fn event(
        state: &mut Self,
        _: &xdg_popup::XdgPopup,
        event: xdg_popup::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_popup::Event::Configure { y, .. } = event {
            state.popup_y = Some(y);
        }
    }
}

impl Dispatch<wl_output::WlOutput, ()> for State {
    fn event(
        state: &mut Self,
        _: &wl_output::WlOutput,
        event: wl_output::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_output::Event::Name { name } = event {
            state.output_name = Some(name);
        }
    }
}

impl Dispatch<wl_callback::WlCallback, ()> for State {
    fn event(
        state: &mut Self,
        _: &wl_callback::WlCallback,
        event: wl_callback::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_callback::Event::Done { .. } = event {
            state.frame_done = true;
        }
    }
}

impl Dispatch<wl_pointer::WlPointer, ()> for State {
    fn event(
        state: &mut Self,
        _: &wl_pointer::WlPointer,
        event: wl_pointer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_pointer::Event::Enter {
                surface_x,
                surface_y,
                ..
            } => {
                state.pointer_inside = true;
                state.pointer_at = Some((surface_x, surface_y));
            }
            wl_pointer::Event::Motion {
                surface_x,
                surface_y,
                ..
            } => state.pointer_at = Some((surface_x, surface_y)),
            wl_pointer::Event::Leave { .. } => state.pointer_inside = false,
            wl_pointer::Event::Button {
                button,
                state: pressed,
                ..
            } => state.buttons.push((
                button,
                pressed == WEnum::Value(wl_pointer::ButtonState::Pressed),
            )),
            _ => {}
        }
    }
}

impl Dispatch<wl_keyboard::WlKeyboard, ()> for State {
    fn event(
        state: &mut Self,
        _: &wl_keyboard::WlKeyboard,
        event: wl_keyboard::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_keyboard::Event::Enter { .. } => state.keyboard_inside = true,
            wl_keyboard::Event::Key { key, .. } => state.keys.push(key),
            _ => {}
        }
    }
}

fn client(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).unwrap();
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 4..=4, ()).unwrap();
    let shm: wl_shm::WlShm = globals.bind(&qh, 1..=1, ()).unwrap();
    let wm: xdg_wm_base::XdgWmBase = globals.bind(&qh, 1..=5, ()).unwrap();
    let seat: wl_seat::WlSeat = globals.bind(&qh, 5..=5, ()).unwrap();
    let _output: wl_output::WlOutput = globals.bind(&qh, 4..=4, ()).unwrap();
    let _pointer = seat.get_pointer(&qh, ());
    let _keyboard = seat.get_keyboard(&qh, ());
    let mut state = State::default();
    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.output_name.as_deref(), Some("t-1"));

    let surface = compositor.create_surface(&qh, ());
    let xs = wm.get_xdg_surface(&surface, &qh, ());
    let _top = xs.get_toplevel(&qh, ());
    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    let buffer = shm_buffer(&shm, &qh, WIDTH, HEIGHT);
    surface.attach(Some(&buffer), 0, 0);
    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("mapped");

    // Answered with nobody on the Rust side present()ing anything.
    let _callback = surface.frame(&qh, ());
    surface.commit();
    while !state.frame_done {
        queue.blocking_dispatch(&mut state).unwrap();
    }
    step.step("frame released");

    // A menu at the bottom edge flips, and the shell was told nothing.
    let positioner = wm.create_positioner(&qh, ());
    positioner.set_size(20, 20);
    positioner.set_anchor_rect(10, 40, 20, 8);
    positioner.set_anchor(xdg_positioner::Anchor::BottomLeft);
    positioner.set_gravity(xdg_positioner::Gravity::BottomRight);
    positioner.set_constraint_adjustment(xdg_positioner::ConstraintAdjustment::FlipY);
    let menu_surface = compositor.create_surface(&qh, ());
    let menu_xs = wm.get_xdg_surface(&menu_surface, &qh, ());
    let _menu = menu_xs.get_popup(Some(&xs), &positioner, &qh, ());
    menu_surface.commit();
    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.popup_y, Some(20));
    step.step("flipped");

    queue.roundtrip(&mut state).unwrap();
    assert!(state.pointer_inside);
    assert_eq!(state.pointer_at, Some((5.0, 6.0)));
    assert_eq!(
        state.buttons,
        [(BTN_LEFT, true), (BTN_LEFT, false)],
        "the click reached the client"
    );
    assert!(state.keyboard_inside, "the press gave it the keyboard");
    assert_eq!(
        state.keys,
        [KEY_A],
        "a binding keeps its key, release included"
    );
    step.step("input seen");
}

#[test]
fn a_whole_compositor_of_policy() {
    let display = Display::new().expect("display");
    let globals = CoreGlobals::new(&display).expect("globals");
    let (shell, seat) = (globals.shell(), globals.seat());
    let layout = OutputLayout::new().expect("layout");
    let popups = PopupTree::new(shell).expect("popups");
    let options = BackendOptions {
        kind: BackendKind::Headless,
        width: WIDTH,
        height: HEIGHT,
        ..BackendOptions::default()
    };
    let backend = AutoBackend::new(&display, &options).expect("backend");
    let renderer = SceneRenderer::new(&backend.vulkan_options()).expect("renderer");
    renderer.set_presentation(Some(globals.presentation()));
    let cursor = Cursor::new(&renderer, &layout, seat).expect("cursor");
    let screens = ScreenManager::new(
        &display,
        &backend,
        &renderer,
        &layout,
        &ScreenOptions {
            background: Color::rgb(0.2, 0.2, 0.2),
            name: "t".into(),
            ..ScreenOptions::default()
        },
    )
    .expect("screens");
    screens.follow_popups(&popups);
    let input = InputRouter::new(seat, &cursor, &screens, shell, &popups).expect("input");
    input.attach(&backend);
    input.bind(Modifiers::LOGO, Keysym::q, QUIT);

    assert!(screens.is_empty());
    backend.start().expect("start");
    assert_eq!(screens.len(), 1);
    assert_eq!(layout.primary_box(), Rect::new(0, 0, WIDTH, HEIGHT));
    assert_eq!(layout.primary_scale(), 1);
    common::dispatch(&display);
    assert!(take_all().iter().any(|e| matches!(e, Event::LayoutChanged)));

    let mut peer = Peer::spawn(&display, client);
    peer.wait(&display, "mapped");
    let surface = take_all()
        .iter()
        .find_map(|e| match e {
            Event::Toplevel(s, ToplevelEvent::Map) => Some(*s),
            _ => None,
        })
        .expect("mapped");
    let t = shell.toplevel(surface).expect("toplevel");
    let scene = vec![SceneItem::toplevel(&t, 0, 0)];
    assert_eq!(scene[0].tag, surface.0);
    screens.set_scene(&scene);
    assert_eq!(
        screens.surface_box(surface),
        Some(Rect::new(0, 0, WIDTH, HEIGHT))
    );
    assert_eq!(screens.surface_box(SurfaceId::INVALID), None);
    peer.go();

    peer.wait(&display, "frame released");
    peer.go();
    peer.wait(&display, "flipped");

    // Everything a compositor would write by hand, from here on nothing but
    // events going through the router.
    input.route(&InputEvent::MotionAbsolute {
        x: 5.0,
        y: 6.0,
        output: None,
    });
    input.route(&InputEvent::Button {
        button: BTN_LEFT,
        pressed: true,
    });
    input.route(&InputEvent::Button {
        button: BTN_LEFT,
        pressed: false,
    });
    let key = |code, pressed, keysym, mods| InputEvent::Key {
        key: code,
        pressed,
        keysym,
        modifiers: Modifiers {
            depressed: mods,
            ..Modifiers::default()
        },
    };
    input.route(&key(KEY_A, true, Keysym::a, 0));
    input.route(&key(KEY_Q, true, Keysym::q, Modifiers::LOGO));
    input.route(&key(KEY_Q, false, Keysym::q, Modifiers::LOGO));
    display.flush_clients();
    common::collect(&display);
    let events = take_all();
    let press = events
        .iter()
        .find_map(|e| match e {
            Event::Button {
                button,
                pressed: true,
                hit,
            } => Some((*button, *hit)),
            _ => None,
        })
        .expect("on_button");
    assert_eq!(press.0, BTN_LEFT);
    assert!(press.1.hit);
    assert_eq!(press.1.tag, surface.0);
    assert_eq!(press.1.surface, Some(surface));
    assert_eq!((press.1.sx, press.1.sy), (5.0, 6.0));
    assert!(
        events.iter().any(|e| matches!(e, Event::KeyBinding(QUIT))),
        "{events:?}"
    );
    peer.go();
    peer.wait(&display, "input seen");
    peer.finish(&display);
}
