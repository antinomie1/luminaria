use std::os::fd::AsRawFd;
use std::time::Duration;

use luminaria::backend::InputEvent;
use luminaria::core::{Display, Error, ErrorKind, KeyRouter};
use luminaria::protocol::Seat;
use luminaria::util::Keysym;

#[test]
fn test_display_creation_and_sockets() {
    let temp_dir = std::env::temp_dir().join("luminaria_test_core_runtime");
    let _ = std::fs::create_dir_all(&temp_dir);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &temp_dir);
    }

    let display = Display::new().expect("failed to create display");
    assert!(display.as_raw_fd() >= 0);

    let socket_name = display
        .add_socket_auto()
        .expect("failed to add auto socket");
    assert!(socket_name.starts_with("wayland-"));

    display.init_shm().expect("failed to init shm");

    let loop_view = display.event_loop();
    assert_eq!(loop_view.as_raw_fd(), display.as_raw_fd());

    // Dispatch with a zero timeout (non-blocking)
    let ret = display.dispatch(Some(Duration::ZERO));
    assert!(ret.is_ok());

    display.flush_clients();
}

#[test]
fn test_key_router() {
    let display = Display::new().expect("display");
    let seat = Seat::new(&display, "seat0").expect("seat");
    let key = |key, pressed| InputEvent::Key {
        key,
        pressed,
        keysym: Keysym::q,
        modifiers: Default::default(),
    };
    let mut router = KeyRouter::new();
    let mut asked = Vec::new();
    // Only presses are offered, and the binding decides per key.
    for event in [key(1, true), key(1, false), key(2, true), key(2, false)] {
        router.route(&seat, &event, |e| {
            let &InputEvent::Key { key, .. } = e else {
                unreachable!()
            };
            asked.push(key);
            key == 1
        });
    }
    assert_eq!(asked, [1, 2]);
    // A press whose release was lost (the VT switched away) is a fresh press.
    router.route(&seat, &key(3, true), |_| true);
    let mut offered = false;
    router.route(&seat, &key(3, true), |_| {
        offered = true;
        false
    });
    assert!(offered);
    // Anything that is not a key is ignored.
    router.route(&seat, &InputEvent::PointerLeave, |_| panic!("not a key"));
}

#[test]
fn test_error_kind_matching() {
    let err = Error::new(ErrorKind::Init, "socket bind failed");
    match err.kind() {
        ErrorKind::Init => {}
        _ => panic!("expected ErrorKind::Init"),
    }
    assert_eq!(err.message(), "socket bind failed");
    assert_eq!(err.to_string(), "socket bind failed");

    // Verify std::error::Error impl
    let std_err: &dyn std::error::Error = &err;
    assert_eq!(std_err.to_string(), "socket bind failed");

    // Verify From<String> and From<&str>
    let from_str: Error = "test error".into();
    assert_eq!(from_str.kind(), ErrorKind::Other);
    assert_eq!(from_str.message(), "test error");
}
