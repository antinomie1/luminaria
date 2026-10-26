//! Tests for wl_compositor signals: NewSurface, Commit, Destroy, Rendered, Invalidated,
//! and Output::PresentEvent.

#[macro_use]
mod common;

use std::time::Duration;

use common::{drain, shm_buffer, ClientSide, Peer};
use luminaria::backend::{HeadlessBackend, OutputEvent};
use luminaria::core::Display;
use luminaria::protocol::{Compositor, CompositorEvent};
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::{wl_buffer, wl_compositor, wl_shm, wl_shm_pool, wl_surface};
use wayland_client::Connection;

struct State;

registry_state!(State);
ignore_events!(State:
    wl_compositor::WlCompositor,
    wl_surface::WlSurface,
    wl_shm::WlShm,
    wl_shm_pool::WlShmPool,
    wl_buffer::WlBuffer,
);

fn client(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let shm: wl_shm::WlShm = globals.bind(&qh, 1..=1, ()).unwrap();
    let mut state = State;

    let surface = compositor.create_surface(&qh, ());
    queue.roundtrip(&mut state).unwrap();
    step.step("surface_created");

    let buffer = shm_buffer(&shm, &qh, 64, 64);
    surface.attach(Some(&buffer), 0, 0);
    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("surface_committed");

    surface.destroy();
    queue.roundtrip(&mut state).unwrap();
    step.step("surface_destroyed");
}

#[test]
fn compositor_surface_lifecycle_events() {
    let display = Display::new().expect("display");
    display.init_shm().expect("shm");
    let compositor = Compositor::new(&display).expect("compositor");

    let mut peer = Peer::spawn(&display, client);

    // 1. Surface created
    peer.wait(&display, "surface_created");
    let events = drain(|| compositor.next_event());
    assert_eq!(events.len(), 1, "expected NewSurface: {events:?}");
    let surface_id = match events[0] {
        CompositorEvent::NewSurface(id) => id,
        other => panic!("expected NewSurface, got {other:?}"),
    };
    assert!(surface_id.is_valid());
    peer.go();

    // 2. Surface committed
    peer.wait(&display, "surface_committed");
    let events = drain(|| compositor.next_event());
    assert_eq!(events.len(), 1, "expected Commit: {events:?}");
    assert_eq!(events[0], CompositorEvent::Commit(surface_id));
    assert!(surface_id.has_buffer());
    assert_eq!(surface_id.surface_width(), 64);

    // 3. Rendered notification
    surface_id.notify_rendered(-1);
    let events = drain(|| compositor.next_event());
    assert_eq!(events.len(), 1, "expected Rendered: {events:?}");
    assert_eq!(events[0], CompositorEvent::Rendered(surface_id));
    peer.go();

    // 4. Surface destroyed
    peer.wait(&display, "surface_destroyed");
    let events = drain(|| compositor.next_event());
    assert_eq!(events.len(), 2, "expected Destroy + Invalidated: {events:?}");
    assert_eq!(events[0], CompositorEvent::Destroy(surface_id));
    assert_eq!(events[1], CompositorEvent::Invalidated(surface_id));

    peer.finish(&display);
}

#[test]
fn output_present_events() {
    let display = Display::new().expect("display");
    let mut backend = HeadlessBackend::new(&display, 10).expect("headless");
    let mut output = backend.add_output(320, 240).expect("output");
    backend.start().expect("start");

    output.schedule_frame();
    assert!(output.frame_scheduled());

    // Dispatch until present arrives
    let deadline = std::time::Instant::now() + Duration::from_secs(2);
    let mut present = None;
    while std::time::Instant::now() < deadline {
        display.dispatch(Some(Duration::from_millis(5))).unwrap();
        if let Some(OutputEvent::Present(p)) = output.next_event() {
            present = Some(p);
            break;
        }
    }

    let p = present.expect("timed out waiting for OutputEvent::Present");
    assert!(p.time_ms() > 0);
    assert!(!p.hw_clock); // Headless uses software clock
}
