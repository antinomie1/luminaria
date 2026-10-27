//! The nested WaylandBackend as an `InputSource`: keymap, modifiers, keys
//! read against the parent's keymap, absolute motion naming its output, leave,
//! scroll, capabilities, and the close request that is not input.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::time::{Duration, Instant};

use luminaria::Event;
use luminaria::backend::{
    InputCapabilities, InputEvent, InputSource, Modifiers, PointerAxis, WaylandBackend,
};
use luminaria::core::Display;
use luminaria::protocol::{Compositor, Seat, XdgEvent, XdgShell};
use luminaria::util::{KeymapState, Keysym};

const KEY_A: u32 = 30;
const SHIFT: u32 = Modifiers::SHIFT;

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
    let closed = Arc::new(AtomicBool::new(false));
    let stop_thread = Arc::clone(&stop);
    let closed_thread = Arc::clone(&closed);
    let socket_clone = socket.clone();

    let child_thread = std::thread::spawn(move || {
        let prev_display = std::env::var_os("WAYLAND_DISPLAY");
        unsafe {
            std::env::set_var("WAYLAND_DISPLAY", &socket_clone);
        }

        let child_display = Display::new().expect("child display");
        let mut backend = WaylandBackend::new(&child_display).expect("backend");
        let child_output = backend.add_output(320, 240).expect("output");
        backend.start().expect("backend start");

        if let Some(p) = prev_display {
            unsafe {
                std::env::set_var("WAYLAND_DISPLAY", p);
            }
        } else {
            unsafe {
                std::env::remove_var("WAYLAND_DISPLAY");
            }
        }

        // The parent's seat, as the backend saw it while connecting.
        let caps = InputSource::capabilities(&backend);
        assert_eq!(
            caps,
            InputCapabilities {
                keyboard: true,
                pointer: true,
                touch: false
            }
        );

        ready_tx.send(child_output.id()).unwrap();

        while !stop_thread.load(Ordering::Relaxed) {
            for event in child_display
                .dispatch(Some(Duration::from_millis(5)))
                .into_iter()
                .flatten()
            {
                match event {
                    Event::Input(ev) => event_tx.send(ev).unwrap(),
                    Event::HostCloseRequest => closed_thread.store(true, Ordering::Relaxed),
                    _ => {}
                }
            }
        }
    });

    // What the host saw, for the toplevel the child creates.
    let mut host_events: Vec<Event> = Vec::new();
    let pump_host = |display: &Display, for_duration: Duration, seen: &mut Vec<Event>| {
        let deadline = Instant::now() + for_duration;
        while Instant::now() < deadline {
            seen.extend(
                display
                    .dispatch(Some(Duration::from_millis(5)))
                    .into_iter()
                    .flatten(),
            );
        }
    };

    // Wait until child thread connects and starts
    let output_id = loop {
        if let Ok(id) = ready_rx.try_recv() {
            break id;
        }
        host_events.extend(
            host_display
                .dispatch(Some(Duration::from_millis(5)))
                .into_iter()
                .flatten(),
        );
    };

    pump_host(&host_display, Duration::from_millis(50), &mut host_events);

    // Find the child's toplevel
    let mut toplevel_id = None;
    for ev in host_events
        .drain(..)
        .filter_map(|e| XdgEvent::try_from(e).ok())
    {
        if let XdgEvent::Toplevel(id, _) = ev {
            toplevel_id = Some(id);
            if let Some(tl) = host_shell.toplevel(id) {
                tl.configure((320, 240));
            }
        }
    }
    let toplevel_id = toplevel_id.expect("expected child to create toplevel on host");

    host_seat.set_keyboard_focus(Some(toplevel_id));
    host_seat.notify_modifiers(Modifiers {
        depressed: 1,
        latched: 2,
        locked: 4,
        group: 0,
    });
    // Shift held, then `a`: the child reads it against the parent's keymap.
    host_seat.notify_modifiers(Modifiers {
        depressed: SHIFT,
        ..Modifiers::default()
    });
    host_seat.notify_key(KEY_A, true);
    host_seat.notify_key(KEY_A, false);
    host_seat.notify_modifiers(Modifiers::default());
    host_seat.pointer_enter(toplevel_id, 10.0, 12.0);
    host_seat.pointer_axis(5.0, 10.0);
    host_seat.pointer_axis_discrete(0, 15);
    host_seat.pointer_axis_stop(true, false);
    host_seat.pointer_clear_focus();

    pump_host(&host_display, Duration::from_millis(50), &mut host_events);

    // Send close
    if let Some(tl) = host_shell.toplevel(toplevel_id) {
        tl.close();
    }
    pump_host(&host_display, Duration::from_millis(50), &mut host_events);

    // Stop child thread
    stop.store(true, Ordering::Relaxed);
    child_thread.join().expect("child thread panicked");

    // Collect all events received by child
    let mut events = Vec::new();
    while let Ok(ev) = event_rx.try_recv() {
        events.push(ev);
    }

    assert!(
        events
            .iter()
            .any(|e| matches!(e, InputEvent::KeymapChange(text) if !text.is_empty())),
        "expected KeymapChange, got: {events:?}"
    );

    assert!(
        events.contains(&InputEvent::Modifiers(Modifiers {
            depressed: 1,
            latched: 2,
            locked: 4,
            group: 0
        })),
        "expected Modifiers event, got: {events:?}"
    );

    assert!(
        events.iter().any(|e| matches!(
            e,
            InputEvent::Key { key: KEY_A, pressed: true, keysym: Keysym::A, modifiers }
                if modifiers.depressed == SHIFT
        )),
        "expected a Shift+a press read as `A`, got: {events:?}"
    );

    assert!(
        events.contains(&InputEvent::MotionAbsolute {
            x: 10.0,
            y: 12.0,
            output: Some(output_id)
        }),
        "expected absolute motion naming the child's output, got: {events:?}"
    );
    assert!(
        events.contains(&InputEvent::PointerLeave),
        "expected PointerLeave, got: {events:?}"
    );

    assert!(
        events.contains(&InputEvent::Axis(PointerAxis {
            dx: 5.0,
            dy: 10.0,
            ..Default::default()
        })),
        "expected smooth Axis event, got: {events:?}"
    );
    assert!(
        events
            .iter()
            .any(|e| matches!(e, InputEvent::Axis(PointerAxis { dy_steps: 15, .. }))),
        "expected discrete Axis event, got: {events:?}"
    );
    assert!(
        events.iter().any(|e| matches!(
            e,
            InputEvent::Axis(PointerAxis {
                stop_horizontal: true,
                ..
            })
        )),
        "expected stop Axis event, got: {events:?}"
    );

    assert!(closed.load(Ordering::Relaxed), "expected the close request");
}
