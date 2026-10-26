//! ext-session-lock events, driven by a real lock client: a lock with its
//! surface from request to orderly unlock, and the lock that fails closed when
//! its client dies holding it.

#[macro_use]
mod common;

use common::{drain, shm_buffer, ClientSide, Peer};
use luminaria::core::Display;
use luminaria::desktop::{SessionLockEvent, SessionLockManager};
use luminaria::protocol::{Compositor, OutputGlobal};
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::{wl_buffer, wl_compositor, wl_output, wl_shm, wl_shm_pool, wl_surface};
use wayland_client::{Connection, Dispatch, QueueHandle};
use wayland_protocols::ext::session_lock::v1::client::{
    ext_session_lock_manager_v1, ext_session_lock_surface_v1, ext_session_lock_v1,
};

#[derive(Default)]
struct State {
    locked: bool,
    finished: bool,
    configures: Vec<(u32, u32)>,
}

registry_state!(State);
ignore_events!(State:
    wl_compositor::WlCompositor,
    wl_surface::WlSurface,
    wl_shm::WlShm,
    wl_shm_pool::WlShmPool,
    wl_buffer::WlBuffer,
    wl_output::WlOutput,
    ext_session_lock_manager_v1::ExtSessionLockManagerV1,
);

impl Dispatch<ext_session_lock_v1::ExtSessionLockV1, ()> for State {
    fn event(
        state: &mut Self,
        _: &ext_session_lock_v1::ExtSessionLockV1,
        event: ext_session_lock_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            ext_session_lock_v1::Event::Locked => state.locked = true,
            ext_session_lock_v1::Event::Finished => state.finished = true,
            _ => {}
        }
    }
}

impl Dispatch<ext_session_lock_surface_v1::ExtSessionLockSurfaceV1, ()> for State {
    fn event(
        state: &mut Self,
        surface: &ext_session_lock_surface_v1::ExtSessionLockSurfaceV1,
        event: ext_session_lock_surface_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let ext_session_lock_surface_v1::Event::Configure { serial, width, height } = event {
            surface.ack_configure(serial);
            state.configures.push((width, height));
        }
    }
}

struct Globals {
    compositor: wl_compositor::WlCompositor,
    shm: wl_shm::WlShm,
    output: wl_output::WlOutput,
    locks: ext_session_lock_manager_v1::ExtSessionLockManagerV1,
}

fn bind(conn: &Connection) -> (Globals, wayland_client::EventQueue<State>, QueueHandle<State>) {
    let (globals, queue) = registry_queue_init::<State>(conn).expect("registry");
    let qh = queue.handle();
    let g = Globals {
        compositor: globals.bind(&qh, 1..=6, ()).unwrap(),
        shm: globals.bind(&qh, 1..=1, ()).unwrap(),
        output: globals.bind(&qh, 1..=4, ()).unwrap(),
        locks: globals.bind(&qh, 1..=1, ()).unwrap(),
    };
    (g, queue, qh)
}

fn orderly_client(conn: Connection, step: ClientSide) {
    let (g, mut queue, qh) = bind(&conn);
    let mut state = State::default();

    let lock = g.locks.lock(&qh, ());
    let surface = g.compositor.create_surface(&qh, ());
    let lock_surface = lock.get_lock_surface(&surface, &g.output, &qh, ());
    queue.roundtrip(&mut state).unwrap();
    step.step("locking");

    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.configures.last(), Some(&(1000, 800)));
    let buffer = shm_buffer(&g.shm, &qh, 1000, 800);
    surface.attach(Some(&buffer), 0, 0);
    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("mapped");

    queue.roundtrip(&mut state).unwrap();
    assert!(state.locked && !state.finished);
    lock_surface.destroy();
    lock.unlock_and_destroy();
    queue.roundtrip(&mut state).unwrap();
    step.step("unlocked");
}

#[test]
fn lock_surface_and_orderly_unlock() {
    let display = Display::new().expect("display");
    display.init_shm().expect("wl_shm");
    let _compositor = Compositor::new(&display).expect("wl_compositor");
    let output = OutputGlobal::new(&display, 1000, 800, "test-1").expect("wl_output");
    let locks = SessionLockManager::new(&display).expect("session-lock");

    let mut peer = Peer::spawn(&display, orderly_client);
    peer.wait(&display, "locking");
    let events = drain(|| locks.next_event());
    let [SessionLockEvent::NewLock(l), SessionLockEvent::NewSurface { lock, surface: s }] = events[..] else {
        panic!("{events:?}");
    };
    assert_eq!(lock, l);
    assert_eq!(locks.current().map(|c| c.id()), Some(l));
    assert_eq!(locks.lock(l).surfaces(), [s]);
    assert!(!locks.is_locked(), "nothing is locked until we say so");
    let ls = locks.lock_surface(s).expect("lock surface");
    assert_eq!(ls.lock(), l);
    assert!(ls.is_on(&output));
    assert!(!ls.is_mapped());
    assert_ne!(ls.configure(1000, 800), 0);
    assert_eq!(ls.size(), (1000, 800));
    peer.go();

    peer.wait(&display, "mapped");
    assert_eq!(drain(|| locks.next_event()), [SessionLockEvent::SurfaceMap(s)]);
    assert!(ls.is_mapped());
    // Every output is covered: now the client may be told.
    locks.lock(l).send_locked();
    assert!(locks.lock(l).is_locked());
    assert!(locks.is_locked());
    peer.go();

    peer.wait(&display, "unlocked");
    assert_eq!(
        drain(|| locks.next_event()),
        [
            SessionLockEvent::SurfaceDestroy(s),
            SessionLockEvent::LockDestroy { lock: l, unlocked: true }
        ]
    );
    assert!(!locks.is_locked());
    assert!(locks.current().is_none());
    assert!(locks.lock_surface(s).is_none());
    peer.finish(&display);
}

fn dying_client(conn: Connection, step: ClientSide) {
    let (g, mut queue, qh) = bind(&conn);
    let mut state = State::default();
    let _lock = g.locks.lock(&qh, ());
    queue.roundtrip(&mut state).unwrap();
    step.step("locking");
    queue.roundtrip(&mut state).unwrap();
    assert!(state.locked);
    // ... and gone, without unlocking.
}

#[test]
fn a_lock_client_that_dies_leaves_the_session_locked() {
    let display = Display::new().expect("display");
    display.init_shm().expect("wl_shm");
    let _compositor = Compositor::new(&display).expect("wl_compositor");
    let _output = OutputGlobal::new(&display, 1000, 800, "test-1").expect("wl_output");
    let locks = SessionLockManager::new(&display).expect("session-lock");

    let mut peer = Peer::spawn(&display, dying_client);
    peer.wait(&display, "locking");
    let events = drain(|| locks.next_event());
    let [SessionLockEvent::NewLock(l)] = events[..] else {
        panic!("{events:?}");
    };
    locks.lock(l).send_locked();
    peer.finish(&display);

    assert_eq!(
        drain(|| locks.next_event()),
        [SessionLockEvent::LockDestroy { lock: l, unlocked: false }]
    );
    assert!(locks.is_locked(), "fails closed");
    assert!(locks.current().is_none());
}
