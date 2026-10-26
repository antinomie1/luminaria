use luminaria::core::Display;
use luminaria::protocol::*;

#[test]
fn test_core_protocols_instantiation() {
    let temp_dir = std::env::temp_dir().join("luminaria_test_protocol_runtime");
    let _ = std::fs::create_dir_all(&temp_dir);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &temp_dir);
    }

    let display = Display::new().expect("failed to create display");

    let _compositor = Compositor::new(&display).expect("Compositor failed");
    let _subcompositor = Subcompositor::new(&display).expect("Subcompositor failed");
    let xdg_shell = XdgShell::new(&display).expect("XdgShell failed");
    xdg_shell.set_bounds(1920, 1080);

    let seat = Seat::new(&display, "seat0").expect("Seat failed");
    seat.set_capabilities(true, true, true);
    assert!(!seat.keymap().is_empty());

    let _layer_shell = LayerShell::new(&display).expect("LayerShell failed");
    let output_global = OutputGlobal::with_properties(
        &display,
        1920,
        1080,
        "test-output",
        (100, 200),
        144000,
        2,
        luminaria::util::Transform::Normal,
    ).expect("OutputGlobal with_properties failed");
    assert_eq!(output_global.position(), (100, 200));
    assert_eq!(output_global.logical_position(), (100, 200));
    assert_eq!(output_global.scale(), 2);
    assert_eq!(output_global.refresh_mhz(), 144000);
    assert_eq!(output_global.transform(), luminaria::util::Transform::Normal);
    assert_eq!(output_global.name(), "test-output");
    assert_eq!(output_global.size(), (1920, 1080));
    assert_eq!(output_global.width(), 1920);
    assert_eq!(output_global.height(), 1080);
    assert_eq!(output_global.logical_size(), (960, 540)); // 1920/2, 1080/2
    assert_eq!(output_global.logical_width(), 960);
    assert_eq!(output_global.logical_height(), 540);

    output_global.set_position(300, 400);
    assert_eq!(output_global.position(), (300, 400));
    output_global.set_scale(1);
    assert_eq!(output_global.scale(), 1);
    assert_eq!(output_global.logical_size(), (1920, 1080));

    let _cursor_theme = CursorTheme::load("default", 24);

    // Protocol Managers
    let _viewporter = Viewporter::new(&display).expect("Viewporter failed");
    let _fractional = FractionalScaleManager::new(&display).expect("FractionalScale failed");
    let _activation = XdgActivation::new(&display).expect("XdgActivation failed");
    let _decoration = XdgDecorationManager::new(&display).expect("XdgDecoration failed");
    let _cursor_shape = CursorShapeManager::new(&display).expect("CursorShape failed");
    let _idle_inhibit = IdleInhibitManager::new(&display).expect("IdleInhibit failed");
    let _idle_notify = IdleNotifier::new(&display).expect("IdleNotifier failed");
    let _single_pixel = SinglePixelBufferManager::new(&display).expect("SinglePixel failed");
    let _tearing = TearingControlManager::new(&display).expect("TearingControl failed");
    let _content_type = ContentTypeManager::new(&display).expect("ContentType failed");
    let _fifo = FifoManager::new(&display).expect("Fifo failed");
    let _commit_timing = CommitTimingManager::new(&display).expect("CommitTiming failed");
    let _presentation = Presentation::new(&display).expect("Presentation failed");
    let _bg_effect = BackgroundEffectManager::new(&display).expect("BackgroundEffect failed");
    let _icon_mgr = XdgToplevelIconManager::new(&display).expect("XdgToplevelIcon failed");

    // Seat globals
    let _data_device = DataDeviceManager::new(&display, &seat).expect("DataDevice failed");
    let _primary_sel = PrimarySelectionManager::new(&display, &seat).expect("PrimarySelection failed");
    let _relative_ptr = RelativePointerManager::new(&display, &seat).expect("RelativePointer failed");
    let _ptr_constraints = PointerConstraints::new(&display, &seat).expect("PointerConstraints failed");
    let _text_input = TextInputManager::new(&display, &seat).expect("TextInput failed");
}
