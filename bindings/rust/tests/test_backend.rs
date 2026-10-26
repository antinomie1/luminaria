use luminaria::backend::{DrmBackend, HeadlessBackend, Session};
use luminaria::core::Display;
use luminaria::util::{Color, Transform};

#[test]
fn test_headless_backend_and_output() {
    let temp_dir = std::env::temp_dir().join("luminaria_test_backend_runtime");
    let _ = std::fs::create_dir_all(&temp_dir);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &temp_dir);
    }

    let display = Display::new().expect("failed to create display");
    let mut backend = HeadlessBackend::new(&display, 16).expect("failed to create headless backend");

    let mut output = backend.add_output(1920, 1080).expect("failed to add output");
    assert_eq!(output.width(), 1920);
    assert_eq!(output.height(), 1080);
    assert_eq!(output.scale(), 1);
    assert_eq!(output.transform(), Transform::Normal);
    assert_eq!(output.logical_width(), 1920);
    assert_eq!(output.logical_height(), 1080);

    // Modify scale & transform
    output.set_scale(2);
    assert_eq!(output.scale(), 2);
    assert_eq!(output.logical_width(), 960);
    assert_eq!(output.logical_height(), 540);

    output.set_transform(Transform::Rotate90);
    assert_eq!(output.transform(), Transform::Rotate90);
    assert_eq!(output.logical_width(), 540);
    assert_eq!(output.logical_height(), 960);

    // Frame scheduling
    assert!(!output.frame_scheduled());
    output.schedule_frame();
    assert!(output.frame_scheduled());

    // Commit solid color
    output.commit(Color::rgb(0.1, 0.2, 0.3)).expect("commit failed");

    // Commit pixel frame (1920x1080 * 4 bytes)
    let pixels = vec![255u8; 1920 * 1080 * 4];
    output.commit_frame(&pixels, 1920, 1080).expect("commit_frame failed");

    // Start backend
    backend.start().expect("start failed");
}

#[test]
fn test_session_handling_skipped_by_default() {
    // Only run if specifically opted-in from a free VT to prevent taking over the host session
    if std::env::var("LUMINARIA_TEST_HARDWARE").as_deref() != Ok("1") {
        eprintln!("skip: session test (set LUMINARIA_TEST_HARDWARE=1 on a free VT)");
        return;
    }

    let display = Display::new().expect("failed to create display");
    if let Ok(session) = Session::new(&display) {
        println!("Session active: {}", session.is_active());
    }
}

#[test]
fn test_drm_backend_handling_skipped_by_default() {
    // Only run if specifically opted-in from a free VT to prevent taking over the host DRM master
    if std::env::var("LUMINARIA_TEST_HARDWARE").as_deref() != Ok("1") {
        eprintln!("skip: DRM backend test (set LUMINARIA_TEST_HARDWARE=1 on a free VT)");
        return;
    }

    let display = Display::new().expect("failed to create display");
    let _ = DrmBackend::new(&display, None, None);
}
