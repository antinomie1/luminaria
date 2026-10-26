//! The escape hatches of docs/api.md §9 that run without a free VT: the DRM
//! device identity, the libinput queue's surface, the output identity and the
//! layout dropping a dead output, and (with `raw-vulkan`) the raw handles.
//! `test_raw_display.rs` covers `Display::as_ptr()`.

use std::fs::File;

use luminaria::backend::{HeadlessBackend, InputCapabilities, LibinputBackend};
use luminaria::core::Display;
use luminaria::gpu::DrmDeviceId;
use luminaria::shell::OutputLayout;

fn runtime_dir() {
    let dir = std::env::temp_dir().join("luminaria_test_escape_hatches_runtime");
    let _ = std::fs::create_dir_all(&dir);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &dir);
    }
}

#[test]
fn drm_device_id_from_fd_and_path() {
    // /dev/null is a character device too, which is all drm_device_id checks:
    // it answers "which node", not "is it a GPU".
    let null = File::open("/dev/null").expect("/dev/null");
    let by_fd = DrmDeviceId::from_fd(&null).expect("a character device has an id");
    assert_eq!(by_fd, DrmDeviceId::from_path("/dev/null").unwrap());
    assert_eq!((by_fd.major, by_fd.minor), (1, 3));

    let regular = File::open(concat!(env!("CARGO_MANIFEST_DIR"), "/Cargo.toml")).unwrap();
    assert_eq!(DrmDeviceId::from_fd(&regular), None);
    assert_eq!(DrmDeviceId::from_path("/nonexistent/card0"), None);
}

#[test]
fn libinput_path_context_starts_empty() {
    runtime_dir();
    let display = Display::new().expect("display");
    // A path context opens only what add_device() names, so starting it grabs
    // nothing and is safe under another compositor.
    let mut input = match LibinputBackend::new_path(&display, None) {
        Ok(input) => input,
        Err(e) => {
            eprintln!("skip: no libinput path context ({e})");
            return;
        }
    };
    input.start().expect("start");
    assert_eq!(input.capabilities(), InputCapabilities::default());
    assert!(!input.keymap().is_empty(), "the console layout compiles");
    assert!(input.add_device("/nonexistent/event0").is_err());
    display.dispatch(Some(std::time::Duration::ZERO)).unwrap();
    assert_eq!(input.next_input_event(), None);

    let keymap = input.keymap();
    assert!(input.set_keymap(&keymap));
    assert!(!input.set_keymap("not a keymap"));
}

#[test]
fn output_identity_and_liveness() {
    runtime_dir();
    let display = Display::new().expect("display");
    let mut backend = HeadlessBackend::new(&display, 16).expect("headless");
    let mut a = backend.add_output(640, 480).unwrap();
    let mut b = backend.add_output(640, 480).unwrap();
    assert_ne!(a.id(), b.id());
    assert!(a.is_alive() && b.is_alive());
    assert!(!a.take_mode_changed());

    let layout = OutputLayout::new().unwrap();
    layout.add_auto(&mut a);
    layout.add_auto(&mut b);
    assert_eq!(layout.output_box(&b).x, 640);
    assert_eq!(layout.bounds().width, 1280);
    layout.remove(&mut b);
    assert_eq!(layout.bounds().width, 640);
    layout.add(&mut b, 640, 0);

    // The monitors go away (here: their backend does). The layout lets go of
    // them by itself, before Rust could have told it to.
    drop(backend);
    assert!(!a.is_alive() && !b.is_alive());
    assert!(layout.bounds().width <= 0, "dead outputs still laid out: {:?}", layout.bounds());
    layout.remove(&mut a); // and a late removal is harmless
}

#[cfg(feature = "raw-vulkan")]
mod raw_vulkan {
    use std::ffi::c_void;

    use luminaria::gpu::{SceneRenderer, VulkanOptions, VulkanRenderer};

    // libvulkan, which the crate links.
    unsafe extern "C" {
        fn vkGetDeviceQueue(device: *mut c_void, family: u32, index: u32, queue: *mut *mut c_void);
    }

    const VK_API_VERSION_1_3: u32 = (1 << 22) | (3 << 12);

    #[test]
    fn handles_are_the_renderers_own() {
        let renderer = match VulkanRenderer::new(&VulkanOptions::default()) {
            Ok(r) => r,
            Err(e) => {
                eprintln!("skip: no Vulkan 1.3 device ({e})");
                return;
            }
        };
        let h = renderer.raw_handles();
        assert!(h.instance != 0 && h.physical_device != 0 && h.device != 0 && h.queue != 0);
        assert_eq!(h.api_version, VK_API_VERSION_1_3);
        // The device and family really are the ones the queue came from.
        let mut queue = std::ptr::null_mut();
        unsafe { vkGetDeviceQueue(h.device as *mut c_void, h.queue_family_index, 0, &mut queue) };
        assert_eq!(queue as u64, h.queue);
        assert_eq!(renderer.raw_handles(), h, "stable across calls");

        let scene = SceneRenderer::new(&VulkanOptions::default()).expect("scene renderer");
        match scene.raw_vulkan_handles() {
            Some(s) => assert!(s.device != 0 && s.device != h.device, "a renderer of its own"),
            None => assert!(!scene.gpu_enabled()),
        }
    }
}
