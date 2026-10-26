use std::os::fd::AsRawFd;
use std::time::Duration;

use luminaria::core::{Display, KeyRouter};

#[test]
fn test_display_creation_and_sockets() {
    let temp_dir = std::env::temp_dir().join("luminaria_test_core_runtime");
    let _ = std::fs::create_dir_all(&temp_dir);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &temp_dir);
    }

    let display = Display::new().expect("failed to create display");
    assert!(display.as_raw_fd() >= 0);

    let socket_name = display.add_socket_auto().expect("failed to add auto socket");
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
    let mut router = KeyRouter::new();
    assert!(!router.is_consumed(1));

    router.consume(1);
    assert!(router.is_consumed(1));

    assert!(router.unconsume(1));
    assert!(!router.is_consumed(1));
    assert!(!router.unconsume(1));
}
