//! Single-window capture wiring: the foreign-toplevel list and the screencopy
//! manager's toplevel source meet through the list's resolver.
use luminaria::core::Display;
use luminaria::desktop::ForeignToplevelList;
use luminaria::protocol::XdgShell;
use luminaria::render::ScreencopyManager;

#[test]
fn toplevel_capture_can_be_enabled_once_there_is_a_list() {
    let temp_dir = std::env::temp_dir().join("luminaria_test_capture_runtime");
    let _ = std::fs::create_dir_all(&temp_dir);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &temp_dir);
    }

    let display = Display::new().expect("failed to create display");
    let shell = XdgShell::new(&display).expect("XdgShell failed");
    let mut list = ForeignToplevelList::new(&display).expect("ForeignToplevelList failed");
    list.track(&shell);
    assert_eq!(list.toplevel_count(), 0);

    let mut screencopy = ScreencopyManager::new(&display).expect("ScreencopyManager failed");
    screencopy
        .enable_toplevel_capture(&display, &list)
        .expect("enabling window capture failed");
    // Enabling again only swaps the resolver.
    screencopy
        .enable_toplevel_capture(&display, &list)
        .expect("enabling twice is fine");
}
