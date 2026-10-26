use luminaria::backend::HeadlessBackend;
use luminaria::core::Display;
use luminaria::gpu::*;
use luminaria::protocol::Seat;
use luminaria::shell::{OutputLayout, SceneItem};
use luminaria::util::{Color, Rect};

#[test]
fn test_scene_renderer_and_output() {
    if std::env::var("LUMINARIA_TEST_HARDWARE").as_deref() != Ok("1") {
        eprintln!("skip: SceneRenderer GPU test (set LUMINARIA_TEST_HARDWARE=1)");
        return;
    }

    let display = Display::new().expect("Display failed");
    let mut backend = HeadlessBackend::new(&display, 16).expect("HeadlessBackend failed");
    let mut output = backend.add_output(800, 600).expect("add_output failed");

    let mut renderer = SceneRenderer::new(&VulkanOptions::default()).expect("SceneRenderer failed");
    let mut state = SceneOutput::new();

    // Reset output target
    renderer.reset(&mut state, &mut output, 0x34325241).expect("reset failed");

    // Present a scene
    let scene = vec![
        SceneItem::rect(Rect::new(0, 0, 800, 600), Color::rgb(0.2, 0.2, 0.2)),
        SceneItem::border(Rect::new(100, 100, 200, 200), 4, Color::rgb(1.0, 1.0, 0.0)),
    ];
    let outcome = renderer.present(&mut state, &mut output, &scene, Color::rgb(0.0, 0.0, 0.0))
        .expect("present failed");
    println!("Scene outcome: {:?}", outcome);

    // Read back frame
    let pixels = renderer.read_back(&mut state).expect("read_back failed");
    assert_eq!(pixels.len(), 800 * 600 * 4);

    // Test cursor integration
    let layout = OutputLayout::new().expect("OutputLayout failed");
    let mut seat = Seat::new(&display, "seat0").expect("Seat failed");
    let mut cursor = Cursor::new(&mut renderer, &layout, &mut seat).expect("Cursor failed");
    assert!(cursor.move_to(400.0, 300.0, Rect::new(0, 0, 800, 600)));
    cursor.leave();
}

#[test]
fn test_direct_scanout_headless() {
    let display = Display::new().expect("Display failed");
    let mut backend = HeadlessBackend::new(&display, 16).expect("HeadlessBackend failed");
    let mut output = backend.add_output(640, 480).expect("add_output failed");

    let ds = DirectScanout::new(&mut output).expect("DirectScanout failed");
    assert!(!ds.available());
    assert_eq!(ds.cached(), 0);
}

#[test]
fn test_vulkan_and_frame_skipped_by_default() {
    // Only run if explicitly opted-in to avoid intrusive GPU operations on the host session
    if std::env::var("LUMINARIA_TEST_HARDWARE").as_deref() != Ok("1") {
        eprintln!("skip: Vulkan GPU hardware test (set LUMINARIA_TEST_HARDWARE=1)");
        return;
    }

    let display = Display::new().expect("Display failed");
    let mut backend = HeadlessBackend::new(&display, 16).expect("HeadlessBackend failed");
    let mut output = backend.add_output(640, 480).expect("add_output failed");

    if let Ok(mut vk) = VulkanRenderer::new(&VulkanOptions::default()) {
        let _ = vk.scanout_modifiers(0x34325241);
        let _dmabuf = LinuxDmabuf::new(&display, Some(&vk));
        let _syncobj = DrmSyncobjManager::new(&display);

        if let Ok(mut frame) = Frame::new(&mut output, &mut vk) {
            let _ = frame.reset(0x34325241);
            frame.begin(Rect::new(0, 0, 640, 480));
            frame.place_rect(Rect::new(10, 10, 50, 50), Color::rgb(1.0, 0.0, 0.0));
            let _ = frame.submit(Color::rgb(0.0, 0.0, 0.0));
        }
    }
}
