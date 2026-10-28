//! Single-window capture wiring: the foreign-toplevel list and the screencopy
//! manager's toplevel source meet through the list's resolver, and the capture
//! path can be handed a GPU.
use luminaria::core::Display;
use luminaria::desktop::ForeignToplevelList;
use luminaria::protocol::XdgShell;
use luminaria::render::{ScreencopyManager, VulkanOptions, VulkanRenderer};

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

/// A renderer turns on the GPU path for window capture — the one that can see a
/// tiled dmabuf. Without a GPU the call has to be safe and simply leave the CPU
/// path in place: a compositor written this way runs on both.
///
/// The renderer is declared BEFORE the manager because the manager borrows it
/// and must be dropped first — the same ordering rule as `LinuxDmabuf` and
/// `Frame`.
#[test]
fn toplevel_capture_accepts_a_renderer() {
    let temp_dir = std::env::temp_dir().join("luminaria_test_capture_runtime");
    let _ = std::fs::create_dir_all(&temp_dir);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &temp_dir);
    }

    let display = Display::new().expect("failed to create display");
    let shell = XdgShell::new(&display).expect("XdgShell failed");
    let mut list = ForeignToplevelList::new(&display).expect("ForeignToplevelList failed");
    list.track(&shell);

    // No Vulkan device on this machine is not a failure: the manager keeps the
    // CPU path, and the call must not panic or hand the renderer a dangling
    // pointer.
    let renderer = VulkanRenderer::new(&VulkanOptions::default());

    let mut screencopy = ScreencopyManager::new(&display).expect("ScreencopyManager failed");
    screencopy
        .enable_toplevel_capture(&display, &list)
        .expect("enabling window capture failed");

    match &renderer {
        Ok(renderer) => {
            screencopy.set_renderer(renderer);
            // Idempotent: a compositor that reconfigures its GPU path calls it
            // again rather than tracking whether it already has.
            screencopy.set_renderer(renderer);
        }
        Err(e) => eprintln!("skip: no Vulkan device ({e}); CPU path stays"),
    }
    drop(screencopy); // before the renderer, which it borrowed
}
