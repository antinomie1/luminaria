use std::time::Duration;

use luminaria::backend::HeadlessBackend;
use luminaria::core::Display;
use luminaria::protocol::{Compositor, Seat, XdgShell};
use luminaria::shell::{CpuCompositor, OutputLayout, SceneItem, SceneKind};
use luminaria::util::{Color, Rect};

#[test]
fn test_custom_compositor_headless_pipeline() {
    let temp_dir = std::env::temp_dir().join("luminaria_test_custom_comp_runtime");
    let _ = std::fs::create_dir_all(&temp_dir);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &temp_dir);
    }

    // 1. Create Display & event loop
    let display = Display::new().expect("failed to create display");
    let socket = display.add_socket_auto().expect("failed to add socket");
    assert!(socket.starts_with("wayland-"));
    display.init_shm().expect("failed to init shm");

    // 2. Setup Headless Backend & Output
    let mut backend = HeadlessBackend::new(&display, 16).expect("failed to create backend");
    let mut output = backend.add_output(1024, 768).expect("failed to add output");
    assert_eq!(output.width(), 1024);
    assert_eq!(output.height(), 768);
    backend.start().expect("backend start failed");

    // 3. Register Core Protocols
    let _compositor = Compositor::new(&display).expect("compositor failed");
    let xdg_shell = XdgShell::new(&display).expect("xdg_shell failed");
    xdg_shell.set_bounds((1024, 768));
    let seat = Seat::new(&display, "seat0").expect("seat failed");
    seat.set_capabilities(true, true, false);

    // 4. Output arrangement in layout
    let layout = OutputLayout::new().expect("output layout failed");
    layout.add(&mut output, 0, 0);
    let out_box = layout.output_box(&output);
    assert_eq!(out_box, Rect::new(0, 0, 1024, 768));

    // 5. Build and composite a scene using software CpuCompositor
    let mut cpu_comp = CpuCompositor::new().expect("cpu compositor failed");
    let items = vec![
        SceneItem::rect(Rect::new(0, 0, 1024, 768), Color::rgb(0.1, 0.1, 0.15)),
        SceneItem::rect(Rect::new(100, 100, 400, 300), Color::rgb(0.2, 0.4, 0.8)),
        SceneItem::border(Rect::new(100, 100, 400, 300), 2, Color::rgb(1.0, 1.0, 1.0)),
    ];

    for item in &items {
        if matches!(item.kind, SceneKind::Rect | SceneKind::Border) {
            cpu_comp.composite_rect(item.rect, item.color);
        }
    }

    let pixels = cpu_comp.finish(1024, 768, Color::rgb(0.0, 0.0, 0.0));
    assert_eq!(pixels.len(), 1024 * 768 * 4);

    // Commit rendered pixels to output
    output.commit_frame(&pixels, 1024, 768).expect("commit_frame failed");

    // 6. Drive the display event loop
    let dispatched = display.dispatch(Some(Duration::ZERO));
    assert!(dispatched.is_ok());

    display.flush_clients();
}
