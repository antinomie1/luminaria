//! The GPU bridge: `import luminaria.gpu;` on the C++ side.
//!
//! Cargo feature `gpu`. Everything here needs Vulkan, libdrm, libinput,
//! libudev, gbm or libseat — the libraries `luminaria.gpu` brings with it — so
//! a consumer that leaves the feature off never compiles this bridge, never
//! builds `luminaria_rs_gpu.cpp` and never links them.
//!
//! Core types are aliased with `type X = crate::ffi::X;` rather than
//! redeclared: cxx then checks that both bridges mean the same C++ type, and
//! Rust sees one type. `crate::ffi_gpu` re-exports enough of them that
//! `src/gpu.rs` can `use crate::ffi_gpu as ffi;` and keep naming `ffi::X`.

#[cxx::bridge(namespace = "lum")]
pub(crate) mod ffi {
    /// A DRM device node's identity: `st_rdev` of /dev/dri/cardN or renderDN.
    /// How the renderer and the display find out they are the same GPU.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    struct DrmDeviceId {
        major: u32,
        minor: u32,
    }

    // Only `raw-vulkan` reads these.
    #[allow(dead_code)]
    #[derive(Debug, Clone, Default)]
    struct VulkanHandlesRaw {
        instance: u64,
        physical_device: u64,
        device: u64,
        queue: u64,
        queue_family: u32,
        api_version: u32,
        device_extensions: Vec<String>,
    }

    unsafe extern "C++" {
        include!("luminaria/luminaria_rs.h");
        include!("luminaria/luminaria_rs_gpu.h");
        // The shared structs (Rect, SceneItemRaw, InputEventRaw) are declared by the core
        // bridge; the C++ side needs its definitions, not just the alias.
        include!("luminaria/src/ffi.rs.h");

        // Core types this bridge's signatures mention.
        type DisplayShim = crate::ffi::DisplayShim;
        type OutputShim = crate::ffi::OutputShim;
        type OutputLayoutShim = crate::ffi::OutputLayoutShim;
        type SeatShim = crate::ffi::SeatShim;
        type CursorShapeManagerShim = crate::ffi::CursorShapeManagerShim;
        type PresentationShim = crate::ffi::PresentationShim;
        type Rect = crate::ffi::Rect;
        type SceneItemRaw = crate::ffi::SceneItemRaw;
        type InputEventRaw = crate::ffi::InputEventRaw;

        // Bare-metal backends: libseat, KMS/DRM, libinput.
        type SessionShim;
        fn session_new(display: &DisplayShim, error: &mut String) -> UniquePtr<SessionShim>;
        fn switch_to(self: &SessionShim, vt: i32, error: &mut String) -> bool;
        fn active(self: &SessionShim) -> bool;
        fn seat_name(self: &SessionShim) -> String;
        fn next_activity(self: &SessionShim) -> u32;

        type DrmBackendShim;
        fn drm_backend_new(display: &DisplayShim, device: &str, session: &SessionShim, error: &mut String) -> UniquePtr<DrmBackendShim>;
        fn drm_backend_new_standalone(display: &DisplayShim, device: &str, error: &mut String) -> UniquePtr<DrmBackendShim>;
        fn start(self: Pin<&mut DrmBackendShim>, error: &mut String) -> bool;
        fn fd(self: &DrmBackendShim) -> i32;
        fn scan_connectors(self: Pin<&mut DrmBackendShim>);
        fn next_event(self: Pin<&mut DrmBackendShim>, kind: &mut u32, removed: &mut u64) -> UniquePtr<OutputShim>;

        type LibinputBackendShim;
        fn libinput_backend_new(display: &DisplayShim, session: &SessionShim, error: &mut String) -> UniquePtr<LibinputBackendShim>;
        fn libinput_backend_new_standalone(display: &DisplayShim, error: &mut String) -> UniquePtr<LibinputBackendShim>;
        fn libinput_backend_new_path(display: &DisplayShim, session: &SessionShim, error: &mut String) -> UniquePtr<LibinputBackendShim>;
        fn libinput_backend_new_path_standalone(display: &DisplayShim, error: &mut String) -> UniquePtr<LibinputBackendShim>;
        fn start(self: Pin<&mut LibinputBackendShim>, error: &mut String) -> bool;
        fn add_device(self: Pin<&mut LibinputBackendShim>, path: &str, error: &mut String) -> bool;
        fn next_input_event(self: Pin<&mut LibinputBackendShim>, out: &mut InputEventRaw) -> bool;
        fn keymap(self: &LibinputBackendShim) -> String;
        fn set_keymap(self: Pin<&mut LibinputBackendShim>, xkb_text: &str) -> bool;
        fn capabilities(self: &LibinputBackendShim) -> u32;

        // The backend luminaria picks (AutoBackend): kind 0 auto, 1 nested, 2 drm, 3 headless.
        type AutoBackendShim;
        fn auto_backend_new(display: &DisplayShim, kind: u32, drm_device: &str, width: i32, height: i32, title: &str, error: &mut String) -> UniquePtr<AutoBackendShim>;
        fn kind(self: &AutoBackendShim) -> u32;
        fn start(self: Pin<&mut AutoBackendShim>, error: &mut String) -> bool;
        fn next_event(self: Pin<&mut AutoBackendShim>, kind: &mut u32, removed: &mut u64) -> UniquePtr<OutputShim>;
        fn next_input_event(self: Pin<&mut AutoBackendShim>, out: &mut InputEventRaw) -> bool;
        fn keymap(self: &AutoBackendShim) -> String;
        fn capabilities(self: &AutoBackendShim) -> u32;
        fn take_close_request(self: Pin<&mut AutoBackendShim>) -> bool;
        fn has_session(self: &AutoBackendShim) -> bool;
        fn session(self: &AutoBackendShim) -> &SessionShim;
        fn session_error(self: &AutoBackendShim) -> String;
        fn drm_device(self: &AutoBackendShim, out: &mut DrmDeviceId) -> bool;

        // GPU protocols.
        type DrmSyncobjManagerShim;
        fn drm_syncobj_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<DrmSyncobjManagerShim>;

        type ScreencopyManagerShim;
        fn screencopy_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<ScreencopyManagerShim>;

        // Vulkan and the scene.
        type VulkanRendererShim;
        fn drm_device_id_from_fd(fd: i32, out: &mut DrmDeviceId) -> bool;
        fn drm_device_id_from_path(path: &str, out: &mut DrmDeviceId) -> bool;
        fn vulkan_renderer_new(has_drm_device: bool, drm_device: &DrmDeviceId, error: &mut String) -> UniquePtr<VulkanRendererShim>;
        fn scanout_modifiers(self: &VulkanRendererShim, drm_format: u32) -> Vec<u64>;
        // Only `raw-vulkan` reads these.
        #[allow(dead_code)]
        fn handles(self: &VulkanRendererShim) -> VulkanHandlesRaw;

        type LinuxDmabufShim;
        fn linux_dmabuf_new(display: &DisplayShim, renderer: &VulkanRendererShim, error: &mut String) -> UniquePtr<LinuxDmabufShim>;
        fn linux_dmabuf_new_headless(display: &DisplayShim, error: &mut String) -> UniquePtr<LinuxDmabufShim>;
        fn linux_dmabuf_new_from_scene_renderer(display: &DisplayShim, renderer: &SceneRendererShim, error: &mut String) -> UniquePtr<LinuxDmabufShim>;

        type SceneOutputShim;
        fn scene_output_new() -> UniquePtr<SceneOutputShim>;
        fn gpu_output(self: &SceneOutputShim) -> bool;

        type SceneRendererShim;
        fn scene_renderer_new(has_drm_device: bool, drm_device: &DrmDeviceId, error: &mut String) -> UniquePtr<SceneRendererShim>;
        fn gpu_enabled(self: &SceneRendererShim) -> bool;
        fn gpu_error(self: &SceneRendererShim) -> String;
        fn reset(self: &SceneRendererShim, state: Pin<&mut SceneOutputShim>, output: Pin<&mut OutputShim>, drm_format: u32, error: &mut String) -> bool;
        fn present(self: &SceneRendererShim, state: Pin<&mut SceneOutputShim>, output: Pin<&mut OutputShim>, items: &[SceneItemRaw], bg_r: f32, bg_g: f32, bg_b: f32, bg_a: f32, error: &mut String) -> i32;
        fn set_presentation(self: &SceneRendererShim, presentation: &PresentationShim);
        fn clear_presentation(self: &SceneRendererShim);
        fn update_cursor(self: &SceneRendererShim);
        fn read_back(self: &SceneRendererShim, state: Pin<&mut SceneOutputShim>, error: &mut String) -> Vec<u8>;
        // Only `raw-vulkan` reads these.
        #[allow(dead_code)]
        fn vulkan_handles(self: &SceneRendererShim, out: &mut VulkanHandlesRaw) -> bool;
        fn set_image(self: &SceneRendererShim, serial: u64, width: i32, height: i32, rgba: &[u8], error: &mut String) -> bool;
        fn remove_image(self: &SceneRendererShim, serial: u64);
        fn clear_images(self: &SceneRendererShim);
        fn has_image(self: &SceneRendererShim, serial: u64) -> bool;

        type CursorShim;
        fn cursor_new(renderer: &SceneRendererShim, layout: &OutputLayoutShim, seat: &SeatShim, error: &mut String) -> UniquePtr<CursorShim>;
        fn cursor_new_with_shapes(renderer: &SceneRendererShim, layout: &OutputLayoutShim, seat: &SeatShim, shapes: &CursorShapeManagerShim, error: &mut String) -> UniquePtr<CursorShim>;
        fn move_to(self: Pin<&mut CursorShim>, x: f64, y: f64, bounds: Rect) -> bool;
        fn move_by(self: Pin<&mut CursorShim>, dx: f64, dy: f64, bounds: Rect) -> bool;
        fn leave(self: Pin<&mut CursorShim>);
        fn refresh(self: Pin<&mut CursorShim>);
        fn position(self: &CursorShim, x: &mut f64, y: &mut f64) -> bool;

        type FrameShim;
        fn frame_new(output: Pin<&mut OutputShim>, renderer: &VulkanRendererShim, error: &mut String) -> UniquePtr<FrameShim>;
        fn reset(self: Pin<&mut FrameShim>, drm_format: u32, error: &mut String) -> bool;
        fn begin(self: Pin<&mut FrameShim>, vx: i32, vy: i32, vw: i32, vh: i32);
        fn place_rect(self: Pin<&mut FrameShim>, x: i32, y: i32, width: i32, height: i32, r: f32, g: f32, b: f32, a: f32);
        fn submit(self: Pin<&mut FrameShim>, bg_r: f32, bg_g: f32, bg_b: f32, bg_a: f32, error: &mut String) -> i32;
        fn send_frame_done(self: Pin<&mut FrameShim>, time_ms: u32);

        type DirectScanoutShim;
        fn direct_scanout_new(output: Pin<&mut OutputShim>) -> UniquePtr<DirectScanoutShim>;
        fn available(self: &DirectScanoutShim) -> bool;
        fn id_for(self: Pin<&mut DirectScanoutShim>, surface_id: u64) -> u32;
        fn committed(self: Pin<&mut DirectScanoutShim>, surface_id: u64);
        fn presented(self: Pin<&mut DirectScanoutShim>);
        fn clear(self: Pin<&mut DirectScanoutShim>);
        fn cached(self: &DirectScanoutShim) -> usize;
    }
}
