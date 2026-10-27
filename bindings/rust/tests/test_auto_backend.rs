//! `AutoBackend`, the twin of tests/test_auto_backend.cpp: headless asked for
//! by name, and auto with no parent compositor and no seat falling all the way
//! through to headless. One test rather than two, because the second rewrites
//! the environment and a test binary runs its tests on parallel threads.

use std::time::Duration;

use luminaria::backend::{AutoBackend, BackendEvent, BackendKind, BackendOptions, Session};
use luminaria::core::Display;

/// One output arrives at start(), the size asked for, and nothing else is there.
fn check_headless(display: &Display, backend: &mut AutoBackend<'_>, width: i32, height: i32) {
    assert_eq!(backend.kind(), BackendKind::Headless);
    assert!(backend.input().is_none());
    assert!(backend.session().is_none());
    assert!(backend.session_error().is_none());
    assert!(!backend.take_close_request());
    assert!(backend.vulkan_options().drm_device.is_none());

    assert!(backend.next_event().is_none(), "nothing before start()");
    backend.start().expect("start");
    display.dispatch(Some(Duration::ZERO)).expect("dispatch");
    let mut outputs = Vec::new();
    while let Some(event) = backend.next_event() {
        match event {
            BackendEvent::OutputAdded(output) => outputs.push(output),
            BackendEvent::OutputRemoved(_) => panic!("headless lost an output"),
        }
    }
    assert_eq!(outputs.len(), 1);
    assert_eq!((outputs[0].width(), outputs[0].height()), (width, height));
}

#[test]
fn test_auto_backend_picks() {
    for name in ["auto", "nested", "drm", "headless"] {
        let kind: BackendKind = name.parse().expect(name);
        assert_eq!(kind.name(), name);
        assert_eq!(kind.to_string(), name);
    }
    assert_eq!("auto".parse::<BackendKind>().unwrap(), BackendKind::Auto);
    assert!("Headless".parse::<BackendKind>().is_err());
    assert!("".parse::<BackendKind>().is_err());

    // Headless by name: whatever else the machine has, it is not tried.
    {
        let display = Display::new().expect("display");
        let options = BackendOptions { kind: BackendKind::Headless, width: 640, height: 480, ..Default::default() };
        let mut backend = AutoBackend::new(&display, &options).expect("headless");
        check_headless(&display, &mut backend, 640, 480);
    }

    // Auto with no parent compositor and no seat. libwayland falls back to
    // wayland-0 without WAYLAND_DISPLAY, so the runtime dir is an empty one;
    // libseat fails outright on a backend name it does not have.
    let runtime = std::env::temp_dir().join(format!("luminaria-auto-backend-{}", std::process::id()));
    std::fs::create_dir_all(&runtime).expect("runtime dir");
    // SAFETY: the only test in this binary, so no other thread reads the
    // environment while it changes.
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &runtime);
        std::env::remove_var("WAYLAND_DISPLAY");
        std::env::remove_var("WAYLAND_SOCKET");
        std::env::set_var("LIBSEAT_BACKEND", "luminaria-test-none");
    }
    {
        let display = Display::new().expect("display");
        if Session::new(&display).is_ok() {
            eprintln!("skip: a libseat session opened anyway; auto would try DRM");
        } else {
            let mut backend = AutoBackend::new(&display, &BackendOptions::default()).expect("auto");
            check_headless(&display, &mut backend, 1280, 800);
        }
    }
    // Asked for by name, the same machine fails instead of falling back.
    {
        let display = Display::new().expect("display");
        let options = BackendOptions { kind: BackendKind::Nested, ..Default::default() };
        assert!(AutoBackend::new(&display, &options).is_err());
    }
    let _ = std::fs::remove_dir(&runtime);
}
