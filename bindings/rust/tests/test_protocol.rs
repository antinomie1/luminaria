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
    let _output_global = OutputGlobal::new(&display, 1920, 1080, "test-output").expect("OutputGlobal failed");
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
