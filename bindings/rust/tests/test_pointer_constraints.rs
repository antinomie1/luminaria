//! pointer-constraints events, driven by a real client: a lock and a
//! confinement from request to destruction, activated only under pointer
//! focus, and a confinement region changed on commit.

#[macro_use]
mod common;

use common::{ClientSide, Peer, take};
use luminaria::core::Display;
use luminaria::protocol::{Compositor, PointerConstraintEvent, PointerConstraintKind, PointerConstraints, Seat};
use luminaria::util::Rect;
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::{wl_compositor, wl_pointer, wl_region, wl_seat, wl_surface};
use wayland_client::{Connection, Dispatch, QueueHandle};
use wayland_protocols::wp::pointer_constraints::zv1::client::{
    zwp_confined_pointer_v1, zwp_locked_pointer_v1, zwp_pointer_constraints_v1,
};

#[derive(Default)]
struct State {
    locked: usize,
    unlocked: usize,
}

registry_state!(State);
ignore_events!(State:
    wl_compositor::WlCompositor,
    wl_surface::WlSurface,
    wl_region::WlRegion,
    wl_seat::WlSeat,
    wl_pointer::WlPointer,
    zwp_pointer_constraints_v1::ZwpPointerConstraintsV1,
    zwp_confined_pointer_v1::ZwpConfinedPointerV1,
);

impl Dispatch<zwp_locked_pointer_v1::ZwpLockedPointerV1, ()> for State {
    fn event(
        state: &mut Self,
        _: &zwp_locked_pointer_v1::ZwpLockedPointerV1,
        event: zwp_locked_pointer_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            zwp_locked_pointer_v1::Event::Locked => state.locked += 1,
            zwp_locked_pointer_v1::Event::Unlocked => state.unlocked += 1,
            _ => {}
        }
    }
}

fn client(conn: Connection, step: ClientSide) {
    use zwp_pointer_constraints_v1::Lifetime;
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let seat: wl_seat::WlSeat = globals.bind(&qh, 1..=7, ()).unwrap();
    let constraints: zwp_pointer_constraints_v1::ZwpPointerConstraintsV1 = globals.bind(&qh, 1..=1, ()).unwrap();
    let mut state = State::default();

    let pointer = seat.get_pointer(&qh, ());
    let game = compositor.create_surface(&qh, ());
    let lock = constraints.lock_pointer(&game, &pointer, None, Lifetime::Persistent, &qh, ());
    let editor = compositor.create_surface(&qh, ());
    let region = compositor.create_region(&qh, ());
    region.add(0, 0, 10, 10);
    let confine = constraints.confine_pointer(&editor, &pointer, Some(&region), Lifetime::Oneshot, &qh, ());
    queue.roundtrip(&mut state).unwrap();
    step.step("requested");

    queue.roundtrip(&mut state).unwrap();
    assert_eq!((state.locked, state.unlocked), (1, 0));
    step.step("locked");

    queue.roundtrip(&mut state).unwrap();
    assert_eq!((state.locked, state.unlocked), (1, 1), "losing focus unlocks");
    lock.set_cursor_position_hint(5.0, 6.0);
    game.commit();
    let wider = compositor.create_region(&qh, ());
    wider.add(0, 0, 20, 20);
    confine.set_region(Some(&wider));
    editor.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("updated");

    lock.destroy();
    confine.destroy();
    queue.roundtrip(&mut state).unwrap();
    step.step("destroyed");
}

#[test]
fn constraints_follow_pointer_focus() {
    let display = Display::new().expect("display");
    let _compositor = Compositor::new(&display).expect("wl_compositor");
    let seat = Seat::new(&display, "seat0").expect("wl_seat");
    seat.set_capabilities(false, true, false);
    let constraints = PointerConstraints::new(&display, &seat).expect("pointer-constraints");

    let mut peer = Peer::spawn(&display, client);
    peer.wait(&display, "requested");
    let events = take::<PointerConstraintEvent>();
    let [PointerConstraintEvent::New(game), PointerConstraintEvent::New(editor)] = events[..] else {
        panic!("{events:?}");
    };
    let lock = constraints.constraint(game).expect("lock");
    assert_eq!(lock.kind(), PointerConstraintKind::Locked);
    assert!(lock.is_persistent());
    assert_eq!(lock.region(), None);
    assert_eq!(lock.cursor_hint(), None);
    let confine = constraints.constraint(editor).expect("confinement");
    assert_eq!(confine.kind(), PointerConstraintKind::Confined);
    assert!(!confine.is_persistent());
    assert_eq!(confine.region(), Some(vec![Rect::new(0, 0, 10, 10)]));

    // Inert without pointer focus: that is the security rule.
    lock.activate();
    assert!(!lock.is_active());
    seat.pointer_enter(game, 1.0, 1.0);
    lock.activate();
    assert!(lock.is_active());
    assert_eq!(constraints.active_constraint(), Some(game));
    peer.go();

    peer.wait(&display, "locked");
    seat.pointer_enter(editor, 1.0, 1.0);
    assert!(!lock.is_active());
    assert_eq!(constraints.active_constraint(), None);
    peer.go();

    peer.wait(&display, "updated");
    assert_eq!(take::<PointerConstraintEvent>(), [PointerConstraintEvent::RegionChange(editor)]);
    assert_eq!(confine.region(), Some(vec![Rect::new(0, 0, 20, 20)]));
    assert_eq!(lock.cursor_hint(), Some((5.0, 6.0)));
    peer.go();

    peer.wait(&display, "destroyed");
    assert_eq!(
        take::<PointerConstraintEvent>(),
        [PointerConstraintEvent::Destroy(game), PointerConstraintEvent::Destroy(editor)]
    );
    assert!(constraints.constraint(game).is_none());
    assert!(!lock.is_active());
    peer.finish(&display);
}
