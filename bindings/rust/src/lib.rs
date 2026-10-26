//! Rust bindings for luminaria, a Wayland compositor library.
//!
//! Luminaria provides three C++23 modules, and this crate mirrors them type for type:
//! - `luminaria`: Core protocols, utilities, event loop, display, and nested/headless backends.
//! - `luminaria.gpu`: Vulkan renderer, DRM/KMS, libinput, session, screencopy, direct scanout, and GPU frame ledgers.
//! - `luminaria.desktop`: Privileged desktop-shell protocols (foreign-toplevel, workspaces, session-lock, output-management, data-control, input-method).
//!
//! A compositor is assembled from the submodules; the policy (where windows go,
//! who has the keyboard, what a key does) stays in the caller:
//! - [`util`]: Rect, Color, Transform, Region, KeymapState, Pixel, resize edges.
//! - [`core`]: Display, EventLoop, Error, KeyRouter.
//! - [`backend`]: Output, HeadlessBackend, WaylandBackend, DrmBackend, LibinputBackend, Session.
//! - [`protocol`]: Compositor, Subcompositor, XdgShell, Seat, LayerShell, and core protocol managers.
//! - [`gpu`]: VulkanRenderer, SceneRenderer, Frame, Cursor, DirectScanout, LinuxDmabuf, Screencopy.
//! - [`desktop`]: ForeignToplevelManager, WorkspaceManager, SessionLockManager, OutputManager, DataControlManager, InputMethodManager.
//! - [`shell`]: OutputLayout, SceneItem, SceneHit, scene_hit_test, CpuCompositor, PopupTree.
//!
//! `examples/minimal.rs` is a complete compositor built this way.

pub mod backend;
pub mod core;
pub mod desktop;
pub mod gpu;
pub mod protocol;
pub mod shell;
pub mod util;

#[cxx::bridge(namespace = "lum")]
pub(crate) mod ffi {
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
    struct Rect {
        x: i32,
        y: i32,
        width: i32,
        height: i32,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    struct Mode {
        width: i32,
        height: i32,
        refresh_mhz: i32,
        preferred: bool,
    }

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

    #[derive(Debug, Clone, Copy)]
    struct ToplevelEventRaw {
        kind: u32,
        surface_id: u64,
    }

    #[derive(Debug, Clone, Copy)]
    struct BackendInputEventRaw {
        kind: u32,
        code: u32,
        pressed: bool,
        x: f64,
        y: f64,
    }

    /// A DRM device node's identity: `st_rdev` of /dev/dri/cardN or renderDN.
    /// How the renderer and the display find out they are the same GPU.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    struct DrmDeviceId {
        major: u32,
        minor: u32,
    }

    #[derive(Debug, Clone, Copy)]
    struct LibinputEventRaw {
        kind: u32,
        code: u32,
        keysym: u32,
        pressed: bool,
        x: f64,
        y: f64,
        steps_x: i32,
        steps_y: i32,
        id: i32,
        flags: u32,
        depressed: u32,
        latched: u32,
        locked: u32,
        group: u32,
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

        // Core & Util
        type DisplayShim;
        fn display_new(error: &mut String) -> UniquePtr<DisplayShim>;
        fn fd(self: &DisplayShim) -> i32;
        fn add_socket_auto(self: &DisplayShim, error: &mut String) -> String;
        fn add_socket(self: &DisplayShim, name: &str, error: &mut String) -> bool;
        fn init_shm(self: &DisplayShim, error: &mut String) -> bool;
        fn run(self: &DisplayShim);
        fn terminate(self: &DisplayShim);
        fn dispatch(self: &DisplayShim, timeout_ms: i32) -> i32;
        fn flush_clients(self: &DisplayShim);
        fn raw_display(self: &DisplayShim) -> usize;

        type KeymapStateShim;
        fn keymap_state_from_layout(layout: &str, error: &mut String) -> UniquePtr<KeymapStateShim>;
        fn keymap_state_from_text(text: &str, error: &mut String) -> UniquePtr<KeymapStateShim>;
        fn text(self: &KeymapStateShim) -> String;
        fn key_keysym(self: &KeymapStateShim, code: u32) -> u32;
        fn update_key(self: Pin<&mut KeymapStateShim>, code: u32, pressed: bool);
        fn modifiers_depressed(self: &KeymapStateShim) -> u32;
        fn modifiers_latched(self: &KeymapStateShim) -> u32;
        fn modifiers_locked(self: &KeymapStateShim) -> u32;
        fn modifiers_group(self: &KeymapStateShim) -> u32;

        type RegionShim;
        fn region_new() -> UniquePtr<RegionShim>;
        fn region_from_box(x: i32, y: i32, width: i32, height: i32) -> UniquePtr<RegionShim>;
        fn add(self: Pin<&mut RegionShim>, x: i32, y: i32, width: i32, height: i32);
        fn subtract_box(self: Pin<&mut RegionShim>, x: i32, y: i32, width: i32, height: i32);
        fn subtract_region(self: Pin<&mut RegionShim>, other: &RegionShim);
        fn intersect_box(self: Pin<&mut RegionShim>, x: i32, y: i32, width: i32, height: i32);
        fn intersect_region(self: Pin<&mut RegionShim>, other: &RegionShim);
        fn empty(self: &RegionShim) -> bool;
        fn clear(self: Pin<&mut RegionShim>);
        fn contains(self: &RegionShim, x: i32, y: i32) -> bool;
        fn extents(self: &RegionShim) -> Rect;
        fn rects(self: &RegionShim) -> Vec<Rect>;

        fn layout_fits_raw(width: i32, height: i32, stride: u32, offset: u32, total_size: usize) -> bool;

        // Backend & Output
        type OutputShim;
        fn width(self: &OutputShim) -> i32;
        fn height(self: &OutputShim) -> i32;
        fn scale(self: &OutputShim) -> i32;
        fn set_scale(self: Pin<&mut OutputShim>, scale: i32);
        fn transform(self: &OutputShim) -> u32;
        fn set_transform(self: Pin<&mut OutputShim>, transform: u32);
        fn logical_width(self: &OutputShim) -> i32;
        fn logical_height(self: &OutputShim) -> i32;
        fn schedule_frame(self: Pin<&mut OutputShim>);
        fn frame_scheduled(self: &OutputShim) -> bool;
        fn modes(self: &OutputShim) -> Vec<Mode>;
        fn current_mode(self: &OutputShim) -> Mode;
        fn set_mode(self: Pin<&mut OutputShim>, width: i32, height: i32, refresh_mhz: i32, error: &mut String) -> bool;
        fn commit(self: Pin<&mut OutputShim>, r: f32, g: f32, b: f32, a: f32, error: &mut String) -> bool;
        fn commit_frame(self: Pin<&mut OutputShim>, rgba: &[u8], width: i32, height: i32, error: &mut String) -> bool;
        fn scanout_modifiers(self: &OutputShim, drm_format: u32) -> Vec<u64>;
        fn has_cursor_plane(self: &OutputShim) -> bool;
        fn move_cursor(self: Pin<&mut OutputShim>, x: i32, y: i32, error: &mut String) -> bool;
        fn hide_cursor(self: Pin<&mut OutputShim>, error: &mut String) -> bool;
        fn take_frame(self: Pin<&mut OutputShim>) -> bool;
        fn take_mode_changed(self: Pin<&mut OutputShim>) -> bool;
        fn alive(self: &OutputShim) -> bool;
        fn id(self: &OutputShim) -> u64;

        type HeadlessBackendShim;
        fn headless_backend_new(display: &DisplayShim, interval_ms: u32, error: &mut String) -> UniquePtr<HeadlessBackendShim>;
        fn add_output(self: Pin<&mut HeadlessBackendShim>, width: i32, height: i32) -> UniquePtr<OutputShim>;
        fn start(self: Pin<&mut HeadlessBackendShim>, error: &mut String) -> bool;

        type WaylandBackendShim;
        fn wayland_backend_new(display: &DisplayShim, error: &mut String) -> UniquePtr<WaylandBackendShim>;
        fn add_output(self: Pin<&mut WaylandBackendShim>, width: i32, height: i32) -> UniquePtr<OutputShim>;
        fn start(self: Pin<&mut WaylandBackendShim>, error: &mut String) -> bool;
        fn keymap(self: &WaylandBackendShim) -> String;
        fn next_input_event(self: Pin<&mut WaylandBackendShim>, out: &mut BackendInputEventRaw) -> bool;

        type SessionShim;
        fn session_new(display: &DisplayShim, error: &mut String) -> UniquePtr<SessionShim>;
        fn switch_to(self: Pin<&mut SessionShim>, vt: i32, error: &mut String) -> bool;
        fn active(self: &SessionShim) -> bool;
        fn seat_name(self: &SessionShim) -> String;
        fn next_activity(self: Pin<&mut SessionShim>) -> u32;

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
        fn next_input_event(self: Pin<&mut LibinputBackendShim>, out: &mut LibinputEventRaw) -> bool;
        fn keymap(self: &LibinputBackendShim) -> String;
        fn set_keymap(self: Pin<&mut LibinputBackendShim>, xkb_text: &str) -> bool;
        fn capabilities(self: &LibinputBackendShim) -> u32;

        // Surface inspection
        fn surface_width(surface_id: u64) -> i32;
        fn surface_height(surface_id: u64) -> i32;
        fn buffer_width(surface_id: u64) -> i32;
        fn buffer_height(surface_id: u64) -> i32;
        fn buffer_scale(surface_id: u64) -> i32;
        fn buffer_transform(surface_id: u64) -> u32;
        fn surface_has_buffer(surface_id: u64) -> bool;
        fn surface_damage(surface_id: u64) -> Vec<Rect>;
        fn surface_send_frame_done(surface_id: u64, time_ms: u32);
        fn surface_accepts_input(surface_id: u64, sx: f64, sy: f64) -> bool;

        // Core Protocols
        type CompositorShim;
        fn compositor_new(display: &DisplayShim, error: &mut String) -> UniquePtr<CompositorShim>;

        type SubcompositorShim;
        fn subcompositor_new(display: &DisplayShim, error: &mut String) -> UniquePtr<SubcompositorShim>;

        type XdgShellShim;
        fn xdg_shell_new(display: &DisplayShim, error: &mut String) -> UniquePtr<XdgShellShim>;
        fn set_bounds(self: Pin<&mut XdgShellShim>, width: i32, height: i32);
        fn next_toplevel_event(self: Pin<&mut XdgShellShim>, out: &mut ToplevelEventRaw) -> bool;
        fn toplevel_configure(self: Pin<&mut XdgShellShim>, surface_id: u64, width: i32, height: i32);
        fn toplevel_set_maximized(self: Pin<&mut XdgShellShim>, surface_id: u64, on: bool);
        fn toplevel_set_activated(self: Pin<&mut XdgShellShim>, surface_id: u64, on: bool);
        fn toplevel_close(self: Pin<&mut XdgShellShim>, surface_id: u64);

        type SeatShim;
        fn seat_new(display: &DisplayShim, name: &str, error: &mut String) -> UniquePtr<SeatShim>;
        fn set_capabilities(self: Pin<&mut SeatShim>, keyboard: bool, pointer: bool, touch: bool);
        fn set_keymap(self: Pin<&mut SeatShim>, xkb_text: &str) -> bool;
        fn keymap(self: &SeatShim) -> String;
        fn set_keyboard_focus(self: Pin<&mut SeatShim>, surface_id: u64);
        fn keyboard_focus(self: &SeatShim) -> u64;
        fn notify_key(self: Pin<&mut SeatShim>, key: u32, pressed: bool);
        fn notify_modifiers(self: Pin<&mut SeatShim>, depressed: u32, latched: u32, locked: u32, group: u32);
        fn pointer_enter(self: Pin<&mut SeatShim>, surface_id: u64, sx: f64, sy: f64);
        fn pointer_clear_focus(self: Pin<&mut SeatShim>);
        fn pointer_focus(self: &SeatShim) -> u64;
        fn pointer_grab(self: &SeatShim) -> u64;
        fn pointer_motion(self: Pin<&mut SeatShim>, sx: f64, sy: f64);
        fn pointer_button(self: Pin<&mut SeatShim>, button: u32, pressed: bool);
        fn pointer_axis(self: Pin<&mut SeatShim>, dx: f64, dy: f64);
        fn pointer_axis_discrete(self: Pin<&mut SeatShim>, dx_steps: i32, dy_steps: i32);
        fn pointer_axis_stop(self: Pin<&mut SeatShim>, horizontal: bool, vertical: bool);
        fn cursor_surface(self: &SeatShim) -> u64;
        fn cursor_hidden(self: &SeatShim) -> bool;
        fn cursor_hotspot_x(self: &SeatShim) -> i32;
        fn cursor_hotspot_y(self: &SeatShim) -> i32;
        fn touch_down(self: Pin<&mut SeatShim>, surface_id: u64, id: i32, x: f64, y: f64);
        fn touch_motion(self: Pin<&mut SeatShim>, id: i32, x: f64, y: f64);
        fn touch_up(self: Pin<&mut SeatShim>, id: i32);
        fn touch_frame(self: Pin<&mut SeatShim>);
        fn touch_cancel(self: Pin<&mut SeatShim>);
        fn touch_focus(self: &SeatShim, id: i32) -> u64;

        type LayerShellShim;
        fn layer_shell_new(display: &DisplayShim, error: &mut String) -> UniquePtr<LayerShellShim>;

        type OutputGlobalShim;
        fn output_global_new(display: &DisplayShim, width: i32, height: i32, name: &str, error: &mut String) -> UniquePtr<OutputGlobalShim>;

        type CursorThemeShim;
        fn cursor_theme_load(name: &str, size: i32, error: &mut String) -> UniquePtr<CursorThemeShim>;

        type ViewporterShim;
        fn viewporter_new(display: &DisplayShim, error: &mut String) -> UniquePtr<ViewporterShim>;

        type FractionalScaleManagerShim;
        fn fractional_scale_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<FractionalScaleManagerShim>;

        type XdgActivationShim;
        fn xdg_activation_new(display: &DisplayShim, error: &mut String) -> UniquePtr<XdgActivationShim>;

        type XdgDecorationManagerShim;
        fn xdg_decoration_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<XdgDecorationManagerShim>;

        type CursorShapeManagerShim;
        fn cursor_shape_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<CursorShapeManagerShim>;

        type IdleInhibitManagerShim;
        fn idle_inhibit_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<IdleInhibitManagerShim>;

        type IdleNotifierShim;
        fn idle_notifier_new(display: &DisplayShim, error: &mut String) -> UniquePtr<IdleNotifierShim>;

        type TextInputManagerShim;
        fn text_input_manager_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<TextInputManagerShim>;

        type SinglePixelBufferManagerShim;
        fn single_pixel_buffer_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<SinglePixelBufferManagerShim>;

        type TearingControlManagerShim;
        fn tearing_control_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<TearingControlManagerShim>;

        type ContentTypeManagerShim;
        fn content_type_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<ContentTypeManagerShim>;

        type FifoManagerShim;
        fn fifo_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<FifoManagerShim>;

        type CommitTimingManagerShim;
        fn commit_timing_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<CommitTimingManagerShim>;

        type PresentationShim;
        fn presentation_new(display: &DisplayShim, error: &mut String) -> UniquePtr<PresentationShim>;

        type BackgroundEffectManagerShim;
        fn background_effect_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<BackgroundEffectManagerShim>;

        type XdgToplevelIconManagerShim;
        fn xdg_toplevel_icon_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<XdgToplevelIconManagerShim>;

        type DrmSyncobjManagerShim;
        fn drm_syncobj_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<DrmSyncobjManagerShim>;

        type ScreencopyManagerShim;
        fn screencopy_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<ScreencopyManagerShim>;

        type ForeignToplevelManagerShim;
        fn foreign_toplevel_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<ForeignToplevelManagerShim>;

        type WorkspaceManagerShim;
        fn workspace_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<WorkspaceManagerShim>;

        type SessionLockManagerShim;
        fn session_lock_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<SessionLockManagerShim>;

        type DataDeviceManagerShim;
        fn data_device_manager_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<DataDeviceManagerShim>;

        type PrimarySelectionManagerShim;
        fn primary_selection_manager_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<PrimarySelectionManagerShim>;

        type RelativePointerManagerShim;
        fn relative_pointer_manager_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<RelativePointerManagerShim>;

        type PointerConstraintsShim;
        fn pointer_constraints_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<PointerConstraintsShim>;

        // GPU Rendering & Shell
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

        type SceneOutputShim;
        fn scene_output_new() -> UniquePtr<SceneOutputShim>;
        fn gpu_output(self: &SceneOutputShim) -> bool;

        type SceneRendererShim;
        fn scene_renderer_new(has_drm_device: bool, drm_device: &DrmDeviceId, error: &mut String) -> UniquePtr<SceneRendererShim>;
        fn gpu_enabled(self: &SceneRendererShim) -> bool;
        fn gpu_error(self: &SceneRendererShim) -> String;
        fn reset(self: Pin<&mut SceneRendererShim>, state: Pin<&mut SceneOutputShim>, output: Pin<&mut OutputShim>, drm_format: u32, error: &mut String) -> bool;
        fn present(self: Pin<&mut SceneRendererShim>, state: Pin<&mut SceneOutputShim>, output: Pin<&mut OutputShim>, items: &[SceneItemRaw], bg_r: f32, bg_g: f32, bg_b: f32, bg_a: f32, error: &mut String) -> i32;
        fn set_presentation(self: Pin<&mut SceneRendererShim>, presentation: &PresentationShim);
        fn clear_presentation(self: Pin<&mut SceneRendererShim>);
        fn update_cursor(self: Pin<&mut SceneRendererShim>);
        fn read_back(self: Pin<&mut SceneRendererShim>, state: Pin<&mut SceneOutputShim>, error: &mut String) -> Vec<u8>;
        // Only `raw-vulkan` reads these.
        #[allow(dead_code)]
        fn vulkan_handles(self: &SceneRendererShim, out: &mut VulkanHandlesRaw) -> bool;

        type OutputLayoutShim;
        fn output_layout_new() -> UniquePtr<OutputLayoutShim>;
        fn add(self: Pin<&mut OutputLayoutShim>, output: Pin<&mut OutputShim>, x: i32, y: i32);
        fn add_auto(self: Pin<&mut OutputLayoutShim>, output: Pin<&mut OutputShim>);
        fn bounds(self: &OutputLayoutShim) -> Rect;
        fn remove(self: Pin<&mut OutputLayoutShim>, output: Pin<&mut OutputShim>);
        fn output_box(self: &OutputLayoutShim, output: &OutputShim) -> Rect;

        type CursorShim;
        fn cursor_new(renderer: Pin<&mut SceneRendererShim>, layout: &OutputLayoutShim, seat: Pin<&mut SeatShim>, error: &mut String) -> UniquePtr<CursorShim>;
        fn move_to(self: Pin<&mut CursorShim>, x: f64, y: f64, bounds: Rect) -> bool;
        fn move_by(self: Pin<&mut CursorShim>, dx: f64, dy: f64, bounds: Rect) -> bool;
        fn leave(self: Pin<&mut CursorShim>);
        fn refresh(self: Pin<&mut CursorShim>);
        fn position(self: &CursorShim, x: &mut f64, y: &mut f64) -> bool;

        type FrameShim;
        fn frame_new(output: Pin<&mut OutputShim>, renderer: Pin<&mut VulkanRendererShim>, error: &mut String) -> UniquePtr<FrameShim>;
        fn reset(self: Pin<&mut FrameShim>, drm_format: u32, error: &mut String) -> bool;
        fn begin(self: Pin<&mut FrameShim>, vx: i32, vy: i32, vw: i32, vh: i32);
        fn place_rect(self: Pin<&mut FrameShim>, x: i32, y: i32, width: i32, height: i32, r: f32, g: f32, b: f32, a: f32);
        fn submit(self: Pin<&mut FrameShim>, bg_r: f32, bg_g: f32, bg_b: f32, bg_a: f32, error: &mut String) -> i32;
        fn send_frame_done(self: Pin<&mut FrameShim>, time_ms: u32);

        type CpuCompositorShim;
        fn cpu_compositor_new() -> UniquePtr<CpuCompositorShim>;
        fn composite_rect(self: Pin<&mut CpuCompositorShim>, x: i32, y: i32, width: i32, height: i32, r: f32, g: f32, b: f32, a: f32);
        fn finish(self: Pin<&mut CpuCompositorShim>, width: i32, height: i32, bg_r: f32, bg_g: f32, bg_b: f32, bg_a: f32) -> Vec<u8>;

        type DirectScanoutShim;
        fn direct_scanout_new(output: Pin<&mut OutputShim>) -> UniquePtr<DirectScanoutShim>;
        fn available(self: &DirectScanoutShim) -> bool;
        fn id_for(self: Pin<&mut DirectScanoutShim>, surface_id: u64) -> u32;
        fn committed(self: Pin<&mut DirectScanoutShim>, surface_id: u64);
        fn presented(self: Pin<&mut DirectScanoutShim>);
        fn clear(self: Pin<&mut DirectScanoutShim>);
        fn cached(self: &DirectScanoutShim) -> usize;

        fn raw_scene_hit_test(items: &[SceneItemRaw], x: f64, y: f64) -> SceneHitRaw;

        // Desktop Protocols
        fn foreign_toplevel_track(ftm: Pin<&mut ForeignToplevelManagerShim>, shell: Pin<&mut XdgShellShim>);

        fn workspace_manager_create_group(wm: Pin<&mut WorkspaceManagerShim>) -> u32;
        fn workspace_manager_create_workspace(wm: Pin<&mut WorkspaceManagerShim>, group: u32, name: &str) -> u32;
        fn workspace_manager_remove_workspace(wm: Pin<&mut WorkspaceManagerShim>, workspace: u32);
        fn workspace_manager_activate_workspace(wm: Pin<&mut WorkspaceManagerShim>, workspace: u32);

        fn session_lock_manager_locked(slm: Pin<&mut SessionLockManagerShim>) -> bool;

        type OutputManagerShim;
        fn output_manager_new(display: &DisplayShim, layout: &OutputLayoutShim, error: &mut String) -> UniquePtr<OutputManagerShim>;
        fn update(self: Pin<&mut OutputManagerShim>);

        type DataControlManagerShim;
        fn data_control_manager_new(display: &DisplayShim, ddm: &DataDeviceManagerShim, error: &mut String) -> UniquePtr<DataControlManagerShim>;

        type InputMethodManagerShim;
        fn input_method_manager_new(display: &DisplayShim, seat: &SeatShim, tim: &TextInputManagerShim, error: &mut String) -> UniquePtr<InputMethodManagerShim>;

        type DesktopGlobalsShim;
        fn desktop_globals_new(display: &DisplayShim, renderer: &VulkanRendererShim, seat_name: &str, error: &mut String) -> UniquePtr<DesktopGlobalsShim>;
        fn desktop_globals_new_headless(display: &DisplayShim, seat_name: &str, error: &mut String) -> UniquePtr<DesktopGlobalsShim>;

        type PopupTreeShim;
        fn popup_tree_new() -> UniquePtr<PopupTreeShim>;
        fn contains(self: &PopupTreeShim, surface_id: u64) -> bool;
        fn dismiss_grabs_outside(self: Pin<&mut PopupTreeShim>, surface_id: u64) -> bool;
    }
}
