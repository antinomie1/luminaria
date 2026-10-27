use luminaria::backend::HeadlessBackend;
use luminaria::core::Display;
use luminaria::gpu::*;
use luminaria::protocol::{Presentation, Seat};
use luminaria::shell::{OutputLayout, SceneItem};
use luminaria::util::{Color, DrmFourcc, Rect};

#[test]
fn test_scene_renderer_and_output() {
    if std::env::var("LUMINARIA_TEST_HARDWARE").as_deref() != Ok("1") {
        eprintln!("skip: SceneRenderer GPU test (set LUMINARIA_TEST_HARDWARE=1)");
        return;
    }

    let display = Display::new().expect("Display failed");
    let mut backend = HeadlessBackend::new(&display, 16).expect("HeadlessBackend failed");
    let mut output = backend.add_output(800, 600).expect("add_output failed");

    let renderer = SceneRenderer::new(&VulkanOptions::default()).expect("SceneRenderer failed");

    // Reset output target
    renderer.reset_with_format(&mut output, DrmFourcc::Argb8888).expect("reset failed");

    // Present a scene
    let scene = vec![
        SceneItem::rect(Rect::new(0, 0, 800, 600), Color::rgb(0.2, 0.2, 0.2)),
        SceneItem::border(Rect::new(100, 100, 200, 200), 4, Color::rgb(1.0, 1.0, 0.0)),
    ];
    let outcome = renderer.present(&mut output, &scene, Color::rgb(0.0, 0.0, 0.0))
        .expect("present failed");
    println!("Scene outcome: {:?}", outcome);

    // Read back frame
    let pixels = renderer.read_back(&output).expect("read_back failed");
    assert_eq!(pixels.len(), 800 * 600 * 4);

    // Test cursor integration
    let layout = OutputLayout::new().expect("OutputLayout failed");
    let seat = Seat::new(&display, "seat0").expect("Seat failed");
    let mut cursor = Cursor::new(&renderer, &layout, &seat).expect("Cursor failed");
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

    if let Ok(vk) = VulkanRenderer::new(&VulkanOptions::default()) {
        let _ = vk.scanout_modifiers(DrmFourcc::Argb8888);
        let _dmabuf = LinuxDmabuf::new(&display, Some(&vk));
        let _syncobj = DrmSyncobjManager::new(&display);

        if let Ok(mut frame) = Frame::new(&mut output, &vk) {
            let _ = frame.reset(DrmFourcc::Argb8888);
            frame.begin(Rect::new(0, 0, 640, 480));
            frame.place_rect(Rect::new(10, 10, 50, 50), Color::rgb(1.0, 0.0, 0.0));
            let _ = frame.submit(Color::rgb(0.0, 0.0, 0.0));
        }
    }
}

#[test]
fn frame_outlives_its_output() {
    // The renderer first, so it drops last: the Frame borrows it.
    let vk = match VulkanRenderer::new(&VulkanOptions::default()) {
        Ok(vk) => vk,
        Err(e) => {
            eprintln!("skip: no Vulkan 1.3 device ({e})");
            return;
        }
    };
    let display = Display::new().expect("Display failed");
    let mut backend = HeadlessBackend::new(&display, 16).expect("HeadlessBackend failed");
    let mut output = backend.add_output(64, 64).expect("add_output failed");
    let mut frame = Frame::new(&mut output, &vk).expect("Frame failed");

    // The monitor goes away (here: its backend does) under a live Frame, which
    // no lifetime can prevent. The frame goes with it and is inert after.
    drop(backend);
    assert!(!output.is_alive());
    assert!(frame.reset(DrmFourcc::Xrgb8888).is_err());
    frame.begin(Rect::new(0, 0, 64, 64));
    frame.place_rect(Rect::new(0, 0, 8, 8), Color::rgb(1.0, 0.0, 0.0));
    assert!(frame.submit(Color::rgb(0.0, 0.0, 0.0)).is_err());
    frame.send_frame_done(0);
}

#[test]
fn presentation_link_survives_either_drop_order() {
    // SceneRenderer::set_presentation takes no borrow (the renderer outlives
    // the Display, so it outlives every global); the link is severed from
    // whichever end goes first. Meaningful under LUMINARIA_SANITIZE=address.
    let renderer = SceneRenderer::new(&VulkanOptions::default()).expect("SceneRenderer failed");
    let display = Display::new().expect("Display failed");

    // The global first, as declaration order makes it in a compositor.
    {
        let first = Presentation::new(&display).expect("Presentation failed");
        let second = Presentation::new(&display).expect("Presentation failed");
        renderer.set_presentation(Some(&first));
        renderer.set_presentation(Some(&second)); // moves the link
        drop(first);
        renderer.set_presentation(None);
        renderer.set_presentation(Some(&second));
    }
    renderer.set_presentation(None);

    // The renderer first.
    let presentation = Presentation::new(&display).expect("Presentation failed");
    {
        let early = SceneRenderer::new(&VulkanOptions::default()).expect("SceneRenderer failed");
        early.set_presentation(Some(&presentation));
    }
    drop(presentation);
}

#[test]
fn test_scene_renderer_images_and_dmabuf() {
    let display = Display::new().expect("Display failed");
    display.init_shm().expect("shm failed");

    let renderer = SceneRenderer::new(&VulkanOptions::default()).expect("SceneRenderer failed");

    // LinuxDmabuf from SceneRenderer
    let dmabuf = LinuxDmabuf::from_scene_renderer(&display, &renderer);
    assert!(dmabuf.is_ok(), "LinuxDmabuf::from_scene_renderer should succeed");
    let dmabuf2 = renderer.create_linux_dmabuf(&display);
    assert!(dmabuf2.is_ok(), "renderer.create_linux_dmabuf should succeed");

    // Test image management
    assert!(!renderer.has_image(42));
    let raw_rgba = vec![255u8; 16 * 16 * 4];
    assert!(renderer.set_image(42, 16, 16, &raw_rgba).is_ok());
    assert!(renderer.has_image(42));

    // Invalid dimensions or size
    assert!(renderer.set_image(43, 0, 16, &raw_rgba).is_err());
    assert!(renderer.set_image(43, 16, 16, &raw_rgba[..10]).is_err());

    let mut backend = HeadlessBackend::new(&display, 16).expect("HeadlessBackend failed");
    let mut output = backend.add_output(320, 240).expect("add_output failed");
    renderer.reset(&mut output).expect("reset failed");

    let scene = vec![
        SceneItem::rect(Rect::new(0, 0, 320, 240), Color::rgb(0.1, 0.1, 0.1)),
        SceneItem::image(Rect::new(10, 10, 16, 16), 42),
    ];
    let outcome = renderer.present(&mut output, &scene, Color::rgb(0.0, 0.0, 0.0));
    assert!(outcome.is_ok(), "present with image item should succeed: {outcome:?}");

    renderer.remove_image(42);
    assert!(!renderer.has_image(42));
    renderer.clear_images();
}
