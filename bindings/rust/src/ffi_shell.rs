//! The shell bridge: `import luminaria.shell;` on the C++ side.
//!
//! Cargo feature `shell` (which implies `backend` and `render`). Everything here
//! is the compositor layer: Frame, SceneRenderer, Cursor, DirectScanout,
//! AutoBackend, popups and layer surfaces.

#[cxx::bridge(namespace = "lum")]
pub(crate) mod ffi {
    #[derive(Debug, Clone, Copy)]
    struct SceneItemRaw {
        kind: u32,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        surface_id: u64,
        surface_x: i32,
        surface_y: i32,
        r: f32,
        g: f32,
        b: f32,
        a: f32,
        thickness: i32,
        serial: u64,
        tag: u64,
        accepts_input: bool,
        alpha: f32,
        corner_radius: f32,
        blur_enabled: bool,
        blur_xray: bool,
        shadow_r: f32,
        shadow_g: f32,
        shadow_b: f32,
        shadow_a: f32,
        shadow_feather: f32,
    }

    #[derive(Debug, Clone, Copy)]
    struct SceneHitRaw {
        hit: bool,
        tag: u64,
        surface_id: u64,
        sx: f64,
        sy: f64,
    }

    unsafe extern "C++" {
        include!("luminaria/luminaria_rs.h");
        include!("luminaria/luminaria_rs_backend.h");
        include!("luminaria/luminaria_rs_render.h");
        include!("luminaria/luminaria_rs_shell.h");
        include!("luminaria/src/ffi.rs.h");
        include!("luminaria/src/ffi_backend.rs.h");
        include!("luminaria/src/ffi_render.rs.h");

        // Types from core bridge
        type DisplayShim = crate::ffi::DisplayShim;
        type OutputShim = crate::ffi::OutputShim;
        type OutputLayoutShim = crate::ffi::OutputLayoutShim;
        type SeatShim = crate::ffi::SeatShim;
        type XdgShellShim = crate::ffi::XdgShellShim;
        type LayerShellShim = crate::ffi::LayerShellShim;
        type CursorShapeManagerShim = crate::ffi::CursorShapeManagerShim;
        type PresentationShim = crate::ffi::PresentationShim;
        type Rect = crate::ffi::Rect;
        type InputEventRaw = crate::ffi::InputEventRaw;

        // Types from backend bridge
        type SessionShim = crate::ffi_backend::SessionShim;

        // Types from render bridge
        type VulkanRendererShim = crate::ffi_render::VulkanRendererShim;
        type DrmDeviceId = crate::ffi_render::DrmDeviceId;
        type VulkanHandlesRaw = crate::ffi_render::VulkanHandlesRaw;
        type LinuxDmabufShim = crate::ffi_render::LinuxDmabufShim;

        // AutoBackend
        type AutoBackendShim;
        fn auto_backend_new(
            display: &DisplayShim,
            kind: u32,
            drm_device: &str,
            width: i32,
            height: i32,
            title: &str,
            error: &mut String,
        ) -> UniquePtr<AutoBackendShim>;
        fn kind(self: &AutoBackendShim) -> u32;
        fn start(self: Pin<&mut AutoBackendShim>, error: &mut String) -> bool;
        fn keymap(self: &AutoBackendShim) -> String;
        fn capabilities(self: &AutoBackendShim) -> u32;
        fn follow_seat(self: &AutoBackendShim, seat: &SeatShim, touch: bool);
        fn has_session(self: &AutoBackendShim) -> bool;
        fn session(self: &AutoBackendShim) -> &SessionShim;
        fn session_error(self: &AutoBackendShim) -> String;
        fn drm_device(self: &AutoBackendShim, out: &mut DrmDeviceId) -> bool;

        // CursorTheme
        type CursorThemeShim;
        fn cursor_theme_load(name: &str, size: i32, error: &mut String) -> UniquePtr<CursorThemeShim>;

        // SceneRenderer
        type SceneRendererShim;
        fn scene_renderer_new(has_drm_device: bool, drm_device: &DrmDeviceId, error: &mut String) -> UniquePtr<SceneRendererShim>;
        fn gpu_enabled(self: &SceneRendererShim) -> bool;
        fn gpu_error(self: &SceneRendererShim) -> String;
        fn reset(self: &SceneRendererShim, output: Pin<&mut OutputShim>, drm_format: u32, error: &mut String) -> bool;
        fn present(
            self: &SceneRendererShim,
            output: Pin<&mut OutputShim>,
            items: &[SceneItemRaw],
            bg_r: f32,
            bg_g: f32,
            bg_b: f32,
            bg_a: f32,
            error: &mut String,
        ) -> i32;
        fn present_in(
            self: &SceneRendererShim,
            layout: &OutputLayoutShim,
            output: Pin<&mut OutputShim>,
            items: &[SceneItemRaw],
            bg_r: f32,
            bg_g: f32,
            bg_b: f32,
            bg_a: f32,
            error: &mut String,
        ) -> i32;
        fn gpu_output(self: &SceneRendererShim, output: &OutputShim) -> bool;
        fn set_presentation(self: &SceneRendererShim, presentation: &PresentationShim);
        fn clear_presentation(self: &SceneRendererShim);
        fn update_cursor(self: &SceneRendererShim);
        fn read_back(self: &SceneRendererShim, output: &OutputShim, error: &mut String) -> Vec<u8>;
        #[allow(dead_code)]
        fn vulkan_handles(self: &SceneRendererShim, out: &mut VulkanHandlesRaw) -> bool;
        fn set_image(self: &SceneRendererShim, serial: u64, width: i32, height: i32, rgba: &[u8], error: &mut String) -> bool;
        fn remove_image(self: &SceneRendererShim, serial: u64);
        fn clear_images(self: &SceneRendererShim);
        fn has_image(self: &SceneRendererShim, serial: u64) -> bool;

        fn linux_dmabuf_new_from_scene_renderer(
            display: &DisplayShim,
            renderer: &SceneRendererShim,
            error: &mut String,
        ) -> UniquePtr<LinuxDmabufShim>;

        // Cursor
        type CursorShim;
        fn cursor_new(
            renderer: &SceneRendererShim,
            layout: &OutputLayoutShim,
            seat: &SeatShim,
            error: &mut String,
        ) -> UniquePtr<CursorShim>;
        fn cursor_new_with_shapes(
            renderer: &SceneRendererShim,
            layout: &OutputLayoutShim,
            seat: &SeatShim,
            shapes: &CursorShapeManagerShim,
            error: &mut String,
        ) -> UniquePtr<CursorShim>;
        fn move_to(self: Pin<&mut CursorShim>, x: f64, y: f64) -> bool;
        fn move_by(self: Pin<&mut CursorShim>, dx: f64, dy: f64) -> bool;
        fn move_to_within(self: Pin<&mut CursorShim>, x: f64, y: f64, bounds: Rect) -> bool;
        fn move_by_within(self: Pin<&mut CursorShim>, dx: f64, dy: f64, bounds: Rect) -> bool;
        fn leave(self: Pin<&mut CursorShim>);
        fn refresh(self: Pin<&mut CursorShim>);
        fn position(self: &CursorShim, x: &mut f64, y: &mut f64) -> bool;

        // Frame
        type FrameShim;
        fn frame_new(output: Pin<&mut OutputShim>, renderer: &VulkanRendererShim, error: &mut String) -> UniquePtr<FrameShim>;
        fn reset(self: Pin<&mut FrameShim>, drm_format: u32, error: &mut String) -> bool;
        fn begin(self: Pin<&mut FrameShim>, vx: i32, vy: i32, vw: i32, vh: i32);
        fn place_rect(self: Pin<&mut FrameShim>, x: i32, y: i32, width: i32, height: i32, r: f32, g: f32, b: f32, a: f32);
        fn submit(self: Pin<&mut FrameShim>, bg_r: f32, bg_g: f32, bg_b: f32, bg_a: f32, error: &mut String) -> i32;
        fn send_frame_done(self: Pin<&mut FrameShim>, time_ms: u32);

        // DirectScanout
        type DirectScanoutShim;
        fn direct_scanout_new(output: Pin<&mut OutputShim>) -> UniquePtr<DirectScanoutShim>;
        fn available(self: &DirectScanoutShim) -> bool;
        fn id_for(self: Pin<&mut DirectScanoutShim>, surface_id: u64) -> u32;
        fn committed(self: Pin<&mut DirectScanoutShim>, surface_id: u64);
        fn presented(self: Pin<&mut DirectScanoutShim>);
        fn clear(self: Pin<&mut DirectScanoutShim>);
        fn cached(self: &DirectScanoutShim) -> usize;

        // PopupTree
        type PopupTreeShim;
        fn popup_tree_new(shell: &XdgShellShim) -> UniquePtr<PopupTreeShim>;
        fn set_parent(self: &PopupTreeShim, shell: &XdgShellShim, popup: u64, parent: u64) -> bool;
        fn append_to(self: &PopupTreeShim, scene: &[SceneItemRaw]) -> Vec<SceneItemRaw>;
        fn contains(self: &PopupTreeShim, surface_id: u64) -> bool;
        fn active_grab(self: &PopupTreeShim) -> u64;
        fn dismiss_grabs_outside(self: Pin<&mut PopupTreeShim>, surface_id: u64) -> bool;

        // LayerManager
        type LayerManagerShim;
        fn layer_manager_new() -> UniquePtr<LayerManagerShim>;
        fn add(self: Pin<&mut LayerManagerShim>, shell: &LayerShellShim, surface_id: u64, output_id: u32) -> bool;
        fn remove(self: Pin<&mut LayerManagerShim>, surface_id: u64);
        fn set_mapped(self: Pin<&mut LayerManagerShim>, surface_id: u64, mapped: bool);
        fn arrange(self: Pin<&mut LayerManagerShim>, output_id: u32, full: Rect, usable: Rect) -> Rect;
        fn append_to(self: &LayerManagerShim, output_id: u32, layer: u32) -> Vec<SceneItemRaw>;
        fn keyboard_target(self: &LayerManagerShim, surface_id: u64) -> u64;
        fn close_output(self: Pin<&mut LayerManagerShim>, output_id: u32);

        // Scene hit testing
        fn raw_scene_hit_test(items: &[SceneItemRaw], x: f64, y: f64) -> SceneHitRaw;
        fn raw_scene_route_pointer(seat: &SeatShim, items: &[SceneItemRaw], x: f64, y: f64) -> SceneHitRaw;
        fn raw_scene_surface_origin(items: &[SceneItemRaw], surface_id: u64, x: &mut f64, y: &mut f64) -> bool;
    }
}
