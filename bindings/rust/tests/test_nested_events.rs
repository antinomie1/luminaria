//! Tests for nested WaylandBackend signals: KeymapChange, ModifiersEvent,
//! PointerAxisEvent, HostCloseRequest.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::Arc;
use std::time::{Duration, Instant};

use luminaria::backend::{BackendInputEvent, Modifiers, WaylandBackend};
use luminaria::core::Display;
use luminaria::protocol::{Compositor, Seat, XdgEvent, XdgShell};
use luminaria::util::KeymapState;

#[test]
fn nested_wayland_backend_events() {
    // 1. Host server setup on main test thread
    let host_display = Display::new().expect("host display");
    let socket = host_display.add_socket_auto().expect("socket");
    host_display.init_shm().expect("shm");
    let _host_comp = Compositor::new(&host_display).expect("compositor");
    let host_shell = XdgShell::new(&host_display).expect("xdg-shell");
    let host_seat = Seat::new(&host_display, "host-seat0").expect("seat");
    host_seat.set_capabilities(true, true, false);

    let keymap = KeymapState::from_layout("us").expect("keymap");
    host_seat.set_keymap(&keymap.text());

    // 2. Child backend thread
    let (ready_tx, ready_rx) = mpsc::channel();
    let (event_tx, event_rx) = mpsc::channel();
    let stop = Arc::new(AtomicBool::new(false));
    let stop_thread = Arc::clone(&stop);
    let socket_clone = socket.clone();

    let child_thread = std::thread::spawn(move || {
        let prev_display = std::env::var_os("WAYLAND_DISPLAY");
        unsafe {
            std::env::set_var("WAYLAND_DISPLAY", &socket_clone);
        }

        let child_display = Display::new().expect("child display");
        let mut backend = WaylandBackend::new(&child_display).expect("backend");
        let _child_output = backend.add_output(320, 240).expect("output");
        backend.start().expect("backend start");

        if let Some(p) = prev_display {
            unsafe { std::env::set_var("WAYLAND_DISPLAY", p); }
        } else {
            unsafe { std::env::remove_var("WAYLAND_DISPLAY"); }
        }

        ready_tx.send(()).unwrap();

        while !stop_thread.load(Ordering::Relaxed) {
            child_display.dispatch(Some(Duration::from_millis(5))).ok();
            while let Some(ev) = backend.next_input_event() {
                event_tx.send(ev).unwrap();
            }
        }
    });

    let pump_host = |display: &Display, for_duration: Duration| {
        let deadline = Instant::now() + for_duration;
        while Instant::now() < deadline {
            display.dispatch(Some(Duration::from_millis(5))).ok();
            display.flush_clients();
        }
    };

    // Wait until child thread connects and starts
    while ready_rx.try_recv().is_err() {
        host_display.dispatch(Some(Duration::from_millis(5))).ok();
        host_display.flush_clients();
    }

    pump_host(&host_display, Duration::from_millis(50));

    // Find the child's toplevel
    let mut toplevel_id = None;
    while let Some(ev) = host_shell.next_event() {
        if let XdgEvent::Toplevel(id, _) = ev {
            toplevel_id = Some(id);
            if let Some(tl) = host_shell.toplevel(id) {
                tl.configure(320, 240);
            }
        }
    }
    let toplevel_id = toplevel_id.expect("expected child to create toplevel on host");

    // Send modifiers & pointer axis
    host_seat.set_keyboard_focus(Some(toplevel_id));
    host_seat.notify_modifiers(1, 2, 4, 0);
    host_seat.pointer_enter(toplevel_id, 10.0, 10.0);
    host_seat.pointer_axis(5.0, 10.0);
    host_seat.pointer_axis_discrete(0, 15);
    host_seat.pointer_axis_stop(true, false);

    pump_host(&host_display, Duration::from_millis(50));

    // Send close
    if let Some(tl) = host_shell.toplevel(toplevel_id) {
        tl.close();
    }
    pump_host(&host_display, Duration::from_millis(50));

    // Stop child thread
    stop.store(true, Ordering::Relaxed);
    child_thread.join().expect("child thread panicked");

    // Collect all events received by child
    let mut events = Vec::new();
    while let Ok(ev) = event_rx.try_recv() {
        events.push(ev);
    }

    // Verify KeymapChange
    assert!(
        events.iter().any(|e| matches!(e, BackendInputEvent::KeymapChange(text) if !text.is_empty())),
        "expected KeymapChange, got: {events:?}"
    );

    // Verify Modifiers
    assert!(
        events.iter().any(|e| matches!(
            e,
            BackendInputEvent::Modifiers(Modifiers {
                depressed: 1,
                latched: 2,
                locked: 4,
                group: 0,
            })
        )),
        "expected Modifiers event, got: {events:?}"
    );

    // Verify Axis
    assert!(
        events.iter().any(|e| matches!(
            e,
            BackendInputEvent::Axis {
                dx,
                dy,
                ..
            } if *dx == 5.0 && *dy == 10.0
        )),
        "expected smooth Axis event, got: {events:?}"
    );

    assert!(
        events.iter().any(|e| matches!(
            e,
            BackendInputEvent::Axis {
                dy_steps: 15,
                ..
            }
        )),
        "expected discrete Axis event, got: {events:?}"
    );

    assert!(
        events.iter().any(|e| matches!(
            e,
            BackendInputEvent::Axis {
                stop_horizontal: true,
                ..
            }
        )),
        "expected stop Axis event, got: {events:?}"
    );

    // Verify HostCloseRequest
    assert!(
        events.iter().any(|e| matches!(e, BackendInputEvent::HostCloseRequest)),
        "expected HostCloseRequest, got: {events:?}"
    );
}
