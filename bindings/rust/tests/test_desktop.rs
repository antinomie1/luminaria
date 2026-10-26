use luminaria::core::Display;
use luminaria::desktop::*;
use luminaria::protocol::*;
use luminaria::shell::OutputLayout;

#[test]
fn test_desktop_protocols() {
    let temp_dir = std::env::temp_dir().join("luminaria_test_desktop_runtime");
    let _ = std::fs::create_dir_all(&temp_dir);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &temp_dir);
    }

    let display = Display::new().expect("failed to create display");
    let seat = Seat::new(&display, "seat0").expect("Seat failed");
    let mut xdg_shell = XdgShell::new(&display).expect("XdgShell failed");
    let layout = OutputLayout::new().expect("OutputLayout failed");
    let ddm = DataDeviceManager::new(&display, &seat).expect("DataDevice failed");
    let tim = TextInputManager::new(&display, &seat).expect("TextInput failed");

    // Foreign toplevel
    let mut ftm = ForeignToplevelManager::new(&display).expect("ForeignToplevel failed");
    ftm.track(&mut xdg_shell);

    // Workspace manager
    let mut wm = WorkspaceManager::new(&display).expect("WorkspaceManager failed");
    let g1 = wm.create_group();
    assert_ne!(g1, 0);
    let ws1 = wm.create_workspace(g1, "1");
    assert_ne!(ws1, 0);
    let ws2 = wm.create_workspace(g1, "2");
    assert_ne!(ws2, 0);
    wm.activate_workspace(ws1);
    wm.remove_workspace(ws2);

    // Session lock manager
    let mut slm = SessionLockManager::new(&display).expect("SessionLockManager failed");
    assert!(!slm.is_locked());

    // Output manager
    let mut om = OutputManager::new(&display, &layout).expect("OutputManager failed");
    om.update();

    // Data control manager
    let _dcm = DataControlManager::new(&display, &ddm).expect("DataControlManager failed");

    // Input method manager
    let _imm = InputMethodManager::new(&display, &seat, &tim).expect("InputMethodManager failed");
}

#[test]
fn test_desktop_globals_suite() {
    let temp_dir = std::env::temp_dir().join("luminaria_test_desktop_runtime");
    let _ = std::fs::create_dir_all(&temp_dir);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &temp_dir);
    }

    let display = Display::new().expect("failed to create display");
    let _globals = DesktopGlobals::new(&display, None, "seat0").expect("DesktopGlobals failed");
}
