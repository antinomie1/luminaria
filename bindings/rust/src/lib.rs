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
//! - [`shell`]: OutputLayout, SceneItem, SceneHit, scene_hit_test, CpuCompositor, PopupTree, LayerManager.
//!
//! The library is driven by its signals, and C++ never calls Rust: each global
//! queues what it emitted, and its `next_event()` hands it over after
//! [`Display::dispatch`](core::Display::dispatch) returns. The [`protocol`]
//! module documentation says how the objects those events are about are named.
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

    /// What an [`EventRaw`] reports. Shared, so both halves name the same values.
    #[derive(Debug)]
    #[repr(u32)]
    enum EventKind {
        None,
        // xdg-shell: `id` is the toplevel's or popup's surface.
        NewToplevel,
        ToplevelMap,
        ToplevelUnmap,
        ToplevelDestroy,
        ToplevelRequestMaximize,
        ToplevelRequestFullscreen,
        ToplevelRequestMinimize,
        ToplevelRequestMove,
        ToplevelRequestResize,
        ToplevelRequestWindowMenu,
        ToplevelIdentityChange,
        ToplevelStateChange,
        ToplevelParentChange,
        NewPopup,
        PopupMap,
        PopupUnmap,
        PopupDestroy,
        PopupReposition,
        PopupGrab,
        // layer-shell: `id` is the layer surface's surface.
        NewLayerSurface,
        LayerSurfaceMap,
        LayerSurfaceUnmap,
        LayerSurfaceDestroy,
        LayerSurfaceStateChange,
        LayerSurfacePopup,
        // ext-session-lock: `id` is the lock, or the lock surface's surface.
        NewSessionLock,
        NewLockSurface,
        LockSurfaceMap,
        LockSurfaceDestroy,
        SessionLockDestroy,
        // idle-inhibit and ext-idle-notify: `id` is the shim's.
        NewIdleInhibitor,
        IdleInhibitorDestroy,
        IdleInhibitChange,
        NewIdleNotification,
        IdleNotificationDestroy,
        IdleStateChange,
        // xdg-activation: `id` is the surface to activate.
        ActivationRequest,
        // pointer-constraints: `id` is the constrained surface.
        NewPointerConstraint,
        PointerConstraintDestroy,
        PointerConstraintRegionChange,
        // text-input-v3: `id` is the shim's.
        NewTextInput,
        TextInputEnable,
        TextInputDisable,
        TextInputCommit,
        TextInputDestroy,
        // input-method-v2: `id` is the shim's, or the popup's surface.
        NewInputMethod,
        InputMethodGrabChange,
        InputMethodDestroy,
        NewInputPopupSurface,
        InputPopupSurfaceDestroy,
        // wl_seat: `id` is the surface, 0 for none.
        SeatKeyboardFocus,
        SeatPointerFocus,
        SeatCursorChange,
        // wl_compositor:
        NewSurface,
        SurfaceCommit,
        SurfaceDestroy,
        SurfaceRendered,
        SurfaceInvalidated,
        // Output:
        OutputPresent,
        // xdg-decoration:
        DecorationRequest,
        // foreign-toplevel:
        ForeignToplevelRequest,
        // ext-workspace:
        WorkspaceRequest,
        // output-management:
        OutputConfigRequest,
        // xdg-toplevel-icon:
        ToplevelIconChange,
        // xdg-activation:
        ActivationTokenRequest,
        // cursor-shape:
        CursorShapeRequest,
    }

    /// One luminaria signal, queued by the shim until Rust drains it. Which
    /// fields mean something depends on `kind`; the wrappers turn it into an enum.
    #[derive(Debug, Clone)]
    struct EventRaw {
        kind: EventKind,
        id: u64,
        other: u64,
        serial: u32,
        x: i32,
        y: i32,
        value: u32,
        flag: bool,
        flag2: bool,
        text: String,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct ToplevelInfoRaw {
        parent: u64,
        mapped: bool,
        min_width: i32,
        min_height: i32,
        max_width: i32,
        max_height: i32,
        geometry: Rect,
        states: u32,
        pending_width: i32,
        pending_height: i32,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct PopupInfoRaw {
        parent: u64,
        mapped: bool,
        has_grab: bool,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
    }

    #[derive(Debug, Clone, Copy)]
    struct PopupAreaRaw {
        surface_id: u64,
        parent: Rect,
        usable: Rect,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct LayerSurfaceInfoRaw {
        layer: u32,
        anchor: u32,
        desired_width: i32,
        desired_height: i32,
        exclusive_zone: i32,
        exclusive_edge: u32,
        margin_top: i32,
        margin_right: i32,
        margin_bottom: i32,
        margin_left: i32,
        keyboard_interactivity: u32,
        mapped: bool,
        pending_width: i32,
        pending_height: i32,
        has_output: bool,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct LockSurfaceInfoRaw {
        lock: u64,
        mapped: bool,
        width: u32,
        height: u32,
        has_output: bool,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct IdleNotificationInfoRaw {
        timeout_ms: u32,
        idled: bool,
        input_only: bool,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct PointerConstraintInfoRaw {
        locked: bool,
        persistent: bool,
        has_region: bool,
        has_hint: bool,
        hint_x: f64,
        hint_y: f64,
        active: bool,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct TextInputInfoRaw {
        focused_surface: u64,
        enabled: bool,
        cursor: u32,
        anchor: u32,
        change_cause: u32,
        content_hint: u32,
        content_purpose: u32,
        cursor_rectangle: Rect,
        serial: u32,
    }

    #[derive(Debug, Clone)]
    struct BackendInputEventRaw {
        kind: u32,
        code: u32,
        pressed: bool,
        x: f64,
        y: f64,
        dx: f64,
        dy: f64,
        dx_steps: i32,
        dy_steps: i32,
        stop_horizontal: bool,
        stop_vertical: bool,
        depressed: u32,
        latched: u32,
        locked: u32,
        group: u32,
        text: String,
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
        fn next_event(self: &OutputShim, out: &mut EventRaw) -> bool;
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
        fn surface_notify_rendered(surface_id: u64, fence_fd: i32);
        fn surface_accepts_input(surface_id: u64, sx: f64, sy: f64) -> bool;

        // Core Protocols
        type CompositorShim;
        fn compositor_new(display: &DisplayShim, error: &mut String) -> UniquePtr<CompositorShim>;
        fn next_event(self: &CompositorShim, out: &mut EventRaw) -> bool;

        type SubcompositorShim;
        fn subcompositor_new(display: &DisplayShim, error: &mut String) -> UniquePtr<SubcompositorShim>;

        type XdgShellShim;
        fn xdg_shell_new(display: &DisplayShim, error: &mut String) -> UniquePtr<XdgShellShim>;
        fn set_bounds(self: &XdgShellShim, width: i32, height: i32);
        fn next_event(self: &XdgShellShim, out: &mut EventRaw) -> bool;
        fn toplevels(self: &XdgShellShim) -> Vec<u64>;
        fn toplevel_info(self: &XdgShellShim, surface_id: u64, out: &mut ToplevelInfoRaw) -> bool;
        fn toplevel_title(self: &XdgShellShim, surface_id: u64) -> String;
        fn toplevel_app_id(self: &XdgShellShim, surface_id: u64) -> String;
        fn toplevel_configure(self: &XdgShellShim, surface_id: u64, width: i32, height: i32) -> u32;
        fn toplevel_set_state(self: &XdgShellShim, surface_id: u64, state: u32, on: bool);
        fn toplevel_close(self: &XdgShellShim, surface_id: u64);
        fn popup_info(self: &XdgShellShim, surface_id: u64, out: &mut PopupInfoRaw) -> bool;
        fn popup_dismiss(self: &XdgShellShim, surface_id: u64);
        fn set_popup_constraint_areas(self: &XdgShellShim, areas: &[PopupAreaRaw]);

        type SeatShim;
        fn seat_new(display: &DisplayShim, name: &str, error: &mut String) -> UniquePtr<SeatShim>;
        fn set_capabilities(self: &SeatShim, keyboard: bool, pointer: bool, touch: bool);
        fn set_keymap(self: &SeatShim, xkb_text: &str) -> bool;
        fn keymap(self: &SeatShim) -> String;
        fn set_keyboard_focus(self: &SeatShim, surface_id: u64);
        fn keyboard_focus(self: &SeatShim) -> u64;
        fn notify_key(self: &SeatShim, key: u32, pressed: bool);
        fn notify_modifiers(self: &SeatShim, depressed: u32, latched: u32, locked: u32, group: u32);
        fn pointer_enter(self: &SeatShim, surface_id: u64, sx: f64, sy: f64);
        fn pointer_clear_focus(self: &SeatShim);
        fn pointer_focus(self: &SeatShim) -> u64;
        fn pointer_grab(self: &SeatShim) -> u64;
        fn pointer_motion(self: &SeatShim, sx: f64, sy: f64);
        fn pointer_button(self: &SeatShim, button: u32, pressed: bool);
        fn pointer_axis(self: &SeatShim, dx: f64, dy: f64);
        fn pointer_axis_discrete(self: &SeatShim, dx_steps: i32, dy_steps: i32);
        fn pointer_axis_stop(self: &SeatShim, horizontal: bool, vertical: bool);
        fn cursor_surface(self: &SeatShim) -> u64;
        fn cursor_hidden(self: &SeatShim) -> bool;
        fn cursor_hotspot_x(self: &SeatShim) -> i32;
        fn cursor_hotspot_y(self: &SeatShim) -> i32;
        fn touch_down(self: &SeatShim, surface_id: u64, id: i32, x: f64, y: f64);
        fn touch_motion(self: &SeatShim, id: i32, x: f64, y: f64);
        fn touch_up(self: &SeatShim, id: i32);
        fn touch_frame(self: &SeatShim);
        fn touch_cancel(self: &SeatShim);
        fn touch_focus(self: &SeatShim, id: i32) -> u64;
        fn next_event(self: &SeatShim, out: &mut EventRaw) -> bool;

        type LayerShellShim;
        fn layer_shell_new(display: &DisplayShim, error: &mut String) -> UniquePtr<LayerShellShim>;
        fn next_event(self: &LayerShellShim, out: &mut EventRaw) -> bool;
        fn layer_surfaces(self: &LayerShellShim) -> Vec<u64>;
        fn info(self: &LayerShellShim, surface_id: u64, out: &mut LayerSurfaceInfoRaw) -> bool;
        fn scope(self: &LayerShellShim, surface_id: u64) -> String;
        fn on_output(self: &LayerShellShim, surface_id: u64, output: &OutputGlobalShim) -> bool;
        fn configure(self: &LayerShellShim, surface_id: u64, width: i32, height: i32) -> u32;
        fn close(self: &LayerShellShim, surface_id: u64);
        fn arrange(self: &LayerShellShim, surface_id: u64, full: Rect, usable: &mut Rect) -> Rect;

        type OutputGlobalShim;
        fn output_global_new(display: &DisplayShim, width: i32, height: i32, name: &str, error: &mut String) -> UniquePtr<OutputGlobalShim>;
        fn set_logical_position(self: &OutputGlobalShim, x: i32, y: i32);
        fn set_mode(self: &OutputGlobalShim, width: i32, height: i32, refresh_mhz: i32);
        fn set_scale(self: &OutputGlobalShim, scale: i32);
        fn set_transform(self: &OutputGlobalShim, transform: u32);
        fn logical_x(self: &OutputGlobalShim) -> i32;
        fn logical_y(self: &OutputGlobalShim) -> i32;
        fn logical_width(self: &OutputGlobalShim) -> i32;
        fn logical_height(self: &OutputGlobalShim) -> i32;
        fn width(self: &OutputGlobalShim) -> i32;
        fn height(self: &OutputGlobalShim) -> i32;
        fn scale(self: &OutputGlobalShim) -> i32;
        fn transform(self: &OutputGlobalShim) -> u32;
        fn refresh_mhz(self: &OutputGlobalShim) -> i32;
        fn name(self: &OutputGlobalShim) -> String;

        type CursorThemeShim;
        fn cursor_theme_load(name: &str, size: i32, error: &mut String) -> UniquePtr<CursorThemeShim>;

        type ViewporterShim;
        fn viewporter_new(display: &DisplayShim, error: &mut String) -> UniquePtr<ViewporterShim>;

        type FractionalScaleManagerShim;
        fn fractional_scale_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<FractionalScaleManagerShim>;

        type XdgActivationShim;
        fn xdg_activation_new(display: &DisplayShim, error: &mut String) -> UniquePtr<XdgActivationShim>;
        fn next_event(self: &XdgActivationShim, out: &mut EventRaw) -> bool;
        fn set_grant_tokens(self: &XdgActivationShim, grant: bool);

        type XdgDecorationManagerShim;
        fn xdg_decoration_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<XdgDecorationManagerShim>;
        fn set_default_mode(self: &XdgDecorationManagerShim, mode: u32);
        fn default_mode(self: &XdgDecorationManagerShim) -> u32;
        fn next_event(self: &XdgDecorationManagerShim, out: &mut EventRaw) -> bool;

        type CursorShapeManagerShim;
        fn cursor_shape_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<CursorShapeManagerShim>;
        fn next_event(self: &CursorShapeManagerShim, out: &mut EventRaw) -> bool;

        type IdleInhibitManagerShim;
        fn idle_inhibit_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<IdleInhibitManagerShim>;
        fn next_event(self: &IdleInhibitManagerShim, out: &mut EventRaw) -> bool;
        fn inhibited(self: &IdleInhibitManagerShim) -> bool;
        fn inhibitor_count(self: &IdleInhibitManagerShim) -> usize;
        fn inhibitor_surface(self: &IdleInhibitManagerShim, inhibitor: u64) -> u64;
        fn inhibitor_visible(self: &IdleInhibitManagerShim, inhibitor: u64) -> bool;
        fn set_inhibitor_visible(self: &IdleInhibitManagerShim, inhibitor: u64, visible: bool);

        type IdleNotifierShim;
        fn idle_notifier_new(display: &DisplayShim, error: &mut String) -> UniquePtr<IdleNotifierShim>;
        fn next_event(self: &IdleNotifierShim, out: &mut EventRaw) -> bool;
        fn notify_activity(self: &IdleNotifierShim);
        fn set_inhibited(self: &IdleNotifierShim, inhibited: bool);
        fn inhibited(self: &IdleNotifierShim) -> bool;
        fn notification_count(self: &IdleNotifierShim) -> usize;
        fn idled_count(self: &IdleNotifierShim) -> usize;
        fn notification_info(self: &IdleNotifierShim, notification: u64, out: &mut IdleNotificationInfoRaw) -> bool;

        type TextInputManagerShim;
        fn text_input_manager_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<TextInputManagerShim>;
        fn next_event(self: &TextInputManagerShim, out: &mut EventRaw) -> bool;
        fn focused(self: &TextInputManagerShim) -> u64;
        fn text_input_info(self: &TextInputManagerShim, text_input: u64, out: &mut TextInputInfoRaw) -> bool;
        fn surrounding_text(self: &TextInputManagerShim, text_input: u64) -> String;
        fn send_preedit_string(self: &TextInputManagerShim, text_input: u64, text: &str, cursor_begin: i32, cursor_end: i32);
        fn send_commit_string(self: &TextInputManagerShim, text_input: u64, text: &str);
        fn send_delete_surrounding_text(self: &TextInputManagerShim, text_input: u64, before: u32, after: u32);
        fn send_done(self: &TextInputManagerShim, text_input: u64);

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
        fn icon_name(self: &XdgToplevelIconManagerShim, surface_id: u64) -> String;
        fn next_event(self: &XdgToplevelIconManagerShim, out: &mut EventRaw) -> bool;

        type DrmSyncobjManagerShim;
        fn drm_syncobj_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<DrmSyncobjManagerShim>;

        type ScreencopyManagerShim;
        fn screencopy_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<ScreencopyManagerShim>;

        type ForeignToplevelManagerShim;
        fn foreign_toplevel_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<ForeignToplevelManagerShim>;
        fn next_event(self: &ForeignToplevelManagerShim, out: &mut EventRaw) -> bool;

        type WorkspaceManagerShim;
        fn workspace_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<WorkspaceManagerShim>;
        fn create_group(self: &WorkspaceManagerShim) -> u32;
        fn create_workspace(self: &WorkspaceManagerShim, group: u32, name: &str) -> u32;
        fn remove_workspace(self: &WorkspaceManagerShim, workspace: u32);
        fn activate_workspace(self: &WorkspaceManagerShim, workspace: u32);
        fn next_event(self: &WorkspaceManagerShim, out: &mut EventRaw) -> bool;

        type SessionLockManagerShim;
        fn session_lock_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<SessionLockManagerShim>;
        fn next_event(self: &SessionLockManagerShim, out: &mut EventRaw) -> bool;
        fn locked(self: &SessionLockManagerShim) -> bool;
        fn current(self: &SessionLockManagerShim) -> u64;
        fn lock_locked(self: &SessionLockManagerShim, lock: u64) -> bool;
        fn send_locked(self: &SessionLockManagerShim, lock: u64);
        fn send_finished(self: &SessionLockManagerShim, lock: u64);
        fn lock_surfaces(self: &SessionLockManagerShim, lock: u64) -> Vec<u64>;
        fn lock_surface_info(self: &SessionLockManagerShim, surface_id: u64, out: &mut LockSurfaceInfoRaw) -> bool;
        fn lock_surface_configure(self: &SessionLockManagerShim, surface_id: u64, width: u32, height: u32) -> u32;
        fn lock_surface_on_output(self: &SessionLockManagerShim, surface_id: u64, output: &OutputGlobalShim) -> bool;

        type DataDeviceManagerShim;
        fn data_device_manager_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<DataDeviceManagerShim>;
        fn next_selection_change(self: &DataDeviceManagerShim, mime_types: &mut Vec<String>) -> bool;
        fn selection_mime_types(self: &DataDeviceManagerShim) -> Vec<String>;
        fn selection_receive(self: &DataDeviceManagerShim, mime: &str, fd: i32) -> bool;

        type PrimarySelectionManagerShim;
        fn primary_selection_manager_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<PrimarySelectionManagerShim>;
        fn next_selection_change(self: &PrimarySelectionManagerShim, mime_types: &mut Vec<String>) -> bool;
        fn selection_mime_types(self: &PrimarySelectionManagerShim) -> Vec<String>;
        fn selection_receive(self: &PrimarySelectionManagerShim, mime: &str, fd: i32) -> bool;

        type RelativePointerManagerShim;
        fn relative_pointer_manager_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<RelativePointerManagerShim>;

        type PointerConstraintsShim;
        fn pointer_constraints_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<PointerConstraintsShim>;
        fn next_event(self: &PointerConstraintsShim, out: &mut EventRaw) -> bool;
        fn constraint_info(self: &PointerConstraintsShim, surface_id: u64, out: &mut PointerConstraintInfoRaw) -> bool;
        fn constraint_region(self: &PointerConstraintsShim, surface_id: u64) -> Vec<Rect>;
        fn activate(self: &PointerConstraintsShim, surface_id: u64);
        fn deactivate(self: &PointerConstraintsShim, surface_id: u64);
        fn active_constraint(self: &PointerConstraintsShim) -> u64;

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

        type OutputLayoutShim;
        fn output_layout_new() -> UniquePtr<OutputLayoutShim>;
        fn add(self: &OutputLayoutShim, output: Pin<&mut OutputShim>, x: i32, y: i32);
        fn add_auto(self: &OutputLayoutShim, output: Pin<&mut OutputShim>);
        fn bounds(self: &OutputLayoutShim) -> Rect;
        fn remove(self: &OutputLayoutShim, output: Pin<&mut OutputShim>);
        fn output_box(self: &OutputLayoutShim, output: &OutputShim) -> Rect;

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
        fn foreign_toplevel_track(ftm: Pin<&mut ForeignToplevelManagerShim>, shell: &XdgShellShim);

        type OutputManagerShim;
        fn output_manager_new(display: &DisplayShim, layout: &OutputLayoutShim, error: &mut String) -> UniquePtr<OutputManagerShim>;
        fn update(self: Pin<&mut OutputManagerShim>);
        fn set_auto_verdict(self: &OutputManagerShim, succeed: bool);
        fn next_event(self: &OutputManagerShim, out: &mut EventRaw) -> bool;

        type DataControlManagerShim;
        fn data_control_manager_new(display: &DisplayShim, ddm: &DataDeviceManagerShim, error: &mut String) -> UniquePtr<DataControlManagerShim>;

        type InputMethodManagerShim;
        fn input_method_manager_new(display: &DisplayShim, seat: &SeatShim, tim: &TextInputManagerShim, error: &mut String) -> UniquePtr<InputMethodManagerShim>;
        fn next_event(self: &InputMethodManagerShim, out: &mut EventRaw) -> bool;
        fn current(self: &InputMethodManagerShim) -> u64;
        fn active(self: &InputMethodManagerShim, input_method: u64) -> bool;
        fn has_keyboard_grab(self: &InputMethodManagerShim, input_method: u64) -> bool;
        fn grab_send_key(self: &InputMethodManagerShim, input_method: u64, time: u32, key: u32, pressed: bool);
        fn grab_send_modifiers(self: &InputMethodManagerShim, input_method: u64, depressed: u32, latched: u32, locked: u32, group: u32);
        fn grab_send_repeat_info(self: &InputMethodManagerShim, input_method: u64, rate: i32, delay: i32);
        fn popup_surfaces(self: &InputMethodManagerShim, input_method: u64) -> Vec<u64>;
        fn popup_text_input_rectangle(self: &InputMethodManagerShim, surface_id: u64) -> Rect;

        type DesktopGlobalsShim;
        fn desktop_globals_new(display: &DisplayShim, renderer: &VulkanRendererShim, seat_name: &str, error: &mut String) -> UniquePtr<DesktopGlobalsShim>;
        fn desktop_globals_new_headless(display: &DisplayShim, seat_name: &str, error: &mut String) -> UniquePtr<DesktopGlobalsShim>;

        type PopupTreeShim;
        fn popup_tree_new() -> UniquePtr<PopupTreeShim>;
        fn add(self: Pin<&mut PopupTreeShim>, shell: &XdgShellShim, popup: u64, parent: u64) -> bool;
        fn set_mapped(self: Pin<&mut PopupTreeShim>, popup: u64, mapped: bool);
        fn remove(self: Pin<&mut PopupTreeShim>, popup: u64);
        fn append_to(self: &PopupTreeShim, scene: &[SceneItemRaw]) -> Vec<SceneItemRaw>;
        fn contains(self: &PopupTreeShim, surface_id: u64) -> bool;
        fn active_grab(self: &PopupTreeShim) -> u64;
        fn dismiss_grabs_outside(self: Pin<&mut PopupTreeShim>, surface_id: u64) -> bool;

        type LayerManagerShim;
        fn layer_manager_new() -> UniquePtr<LayerManagerShim>;
        fn add(self: Pin<&mut LayerManagerShim>, shell: &LayerShellShim, surface_id: u64, output_id: u32) -> bool;
        fn remove(self: Pin<&mut LayerManagerShim>, surface_id: u64);
        fn set_mapped(self: Pin<&mut LayerManagerShim>, surface_id: u64, mapped: bool);
        fn arrange(self: Pin<&mut LayerManagerShim>, output_id: u32, full: Rect, usable: Rect) -> Rect;
        fn append_to(self: &LayerManagerShim, output_id: u32, layer: u32) -> Vec<SceneItemRaw>;
        fn keyboard_target(self: &LayerManagerShim, surface_id: u64) -> u64;
        fn close_output(self: Pin<&mut LayerManagerShim>, output_id: u32);
    }
}

impl ffi::EventRaw {
    fn empty() -> Self {
        Self {
            kind: ffi::EventKind::None,
            id: 0,
            other: 0,
            serial: 0,
            x: 0,
            y: 0,
            value: 0,
            flag: false,
            flag2: false,
            text: String::new(),
        }
    }
}

/// Pops the shim's next queued event, if any.
pub(crate) fn next_raw_event(next: impl FnOnce(&mut ffi::EventRaw) -> bool) -> Option<ffi::EventRaw> {
    let mut raw = ffi::EventRaw::empty();
    next(&mut raw).then_some(raw)
}
