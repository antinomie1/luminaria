//! wl_seat events, driven by a real client: keyboard and pointer focus moved
//! by the compositor, and the cursor the client sets and then hides.

#[macro_use]
mod common;

use common::{shm_buffer, ClientSide, Peer, take};
use luminaria::core::Display;
use luminaria::protocol::{Compositor, Seat, SeatEvent, ToplevelEvent, XdgEvent, XdgShell};
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::{wl_buffer, wl_compositor, wl_keyboard, wl_pointer, wl_seat, wl_shm, wl_shm_pool, wl_surface};
use wayland_client::{Connection, Dispatch, QueueHandle};
use wayland_protocols::xdg::shell::client::xdg_wm_base;

#[derive(Default)]
struct State {
    enter_serial: Option<u32>,
}

registry_state!(State);
xdg_basics!(State);
ignore_events!(State:
    wl_compositor::WlCompositor,
    wl_surface::WlSurface,
    wl_seat::WlSeat,
    wl_keyboard::WlKeyboard,
    wl_shm::WlShm,
    wl_shm_pool::WlShmPool,
    wl_buffer::WlBuffer,
);

impl Dispatch<wl_pointer::WlPointer, ()> for State {
    fn event(state: &mut Self, _: &wl_pointer::WlPointer, event: wl_pointer::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {
        if let wl_pointer::Event::Enter { serial, .. } = event {
            state.enter_serial = Some(serial);
        }
    }
}

fn client(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let shm: wl_shm::WlShm = globals.bind(&qh, 1..=1, ()).unwrap();
    let wm: xdg_wm_base::XdgWmBase = globals.bind(&qh, 1..=6, ()).unwrap();
    let seat: wl_seat::WlSeat = globals.bind(&qh, 1..=7, ()).unwrap();
    let mut state = State::default();

    let surface = compositor.create_surface(&qh, ());
    let xs = wm.get_xdg_surface(&surface, &qh, ());
    let _toplevel = xs.get_toplevel(&qh, ());
    surface.commit();
    let _keyboard = seat.get_keyboard(&qh, ());
    let pointer = seat.get_pointer(&qh, ());
    queue.roundtrip(&mut state).unwrap();
    step.step("ready");

    queue.roundtrip(&mut state).unwrap();
    let serial = state.enter_serial.expect("no pointer enter");
    let cursor = compositor.create_surface(&qh, ());
    let image = shm_buffer(&shm, &qh, 16, 16);
    cursor.attach(Some(&image), 0, 0);
    cursor.commit();
    pointer.set_cursor(serial, Some(&cursor), 3, 4);
    queue.roundtrip(&mut state).unwrap();
    step.step("cursor");

    pointer.set_cursor(serial, None, 0, 0);
    queue.roundtrip(&mut state).unwrap();
    step.step("hidden");
}

#[test]
fn focus_and_cursor_changes() {
    let display = Display::new().expect("display");
    display.init_shm().expect("wl_shm");
    let _compositor = Compositor::new(&display).expect("wl_compositor");
    let _shell = XdgShell::new(&display).expect("xdg-shell");
    let seat = Seat::new(&display, "seat0").expect("wl_seat");
    seat.set_capabilities(true, true, false);

    let mut peer = Peer::spawn(&display, client);
    peer.wait(&display, "ready");
    let s = match take::<XdgEvent>().first() {
        Some(&XdgEvent::Toplevel(s, ToplevelEvent::Created)) => s,
        other => panic!("{other:?}"),
    };
    assert_eq!(take::<SeatEvent>(), []);
    seat.set_keyboard_focus(Some(s));
    seat.pointer_enter(s, 10.0, 10.0);
    common::collect(&display);
    assert_eq!(
        take::<SeatEvent>(),
        [SeatEvent::KeyboardFocus(Some(s)), SeatEvent::PointerFocus(Some(s))]
    );
    peer.go();

    peer.wait(&display, "cursor");
    let events = take::<SeatEvent>();
    let [SeatEvent::CursorChange { surface: Some(c), hotspot: (3, 4), hidden: false }] = events[..] else {
        panic!("{events:?}");
    };
    assert_ne!(c, s);
    assert_eq!(seat.cursor_surface(), Some(c));
    assert_eq!(seat.cursor_hotspot(), (3, 4));
    peer.go();

    peer.wait(&display, "hidden");
    let events = take::<SeatEvent>();
    let [SeatEvent::CursorChange { surface: None, hidden: true, .. }] = events[..] else {
        panic!("{events:?}");
    };
    assert!(seat.cursor_hidden());

    // The pointer leaving takes the client's cursor with it: back to the
    // compositor's own, which is neither a surface nor hidden.
    seat.pointer_clear_focus();
    seat.set_keyboard_focus(None);
    common::collect(&display);
    assert_eq!(
        take::<SeatEvent>(),
        [
            SeatEvent::CursorChange { surface: None, hotspot: (0, 0), hidden: false },
            SeatEvent::PointerFocus(None),
            SeatEvent::KeyboardFocus(None)
        ]
    );
    peer.finish(&display);
}
