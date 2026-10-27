//! The core bridge: `import luminaria;` on the C++ side.
//!
//! One `#[cxx::bridge]` per C++ module (ADR 0003). This one holds the plain
//! data every bridge shares — `Rect`, `EventRaw` and its `EventKind`, the scene
//! item — plus everything the core module provides: utilities, the headless and
//! nested backends, the protocols and the shell primitives.
//!
//! The other bridges alias these types with `type X = crate::ffi::X;` instead of
//! redeclaring them, so one Rust type and one C++ type sit behind both bridges.

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

    /// One entry of [`SurfaceId::surface_tree`](crate::protocol::SurfaceId::surface_tree):
    /// a surface of the tree and where it sits relative to the root.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct SurfaceAtRaw {
        surface_id: u64,
        x: i32,
        y: i32,
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
        // Output: `id` is the output's.
        OutputPresent,
        OutputFrame,
        OutputModeChanged,
        // Backends: `id` is the output's.
        OutputAdded,
        OutputRemoved,
        HostCloseRequest,
        SessionActive,
        SessionInactive,
        // Selections: `text` is the new owner's mime types, one per line.
        SelectionChange,
        PrimarySelectionChange,
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

    /// Which `lm::InputEvent` alternative an [`InputEventRaw`] carries. Both
    /// input sources (nested here, libinput in the GPU bridge) fill the same one.
    #[derive(Debug)]
    #[repr(u32)]
    enum InputEventKind {
        Key,
        Modifiers,
        KeymapChange,
        Motion,
        MotionAbsolute,
        PointerLeave,
        Button,
        Axis,
        TouchDown,
        TouchMotion,
        TouchUp,
        TouchFrame,
        TouchCancel,
        Capabilities,
    }

    /// `kind` says which of the other fields mean something. `x`/`y` are the
    /// motion, axis or touch position; `steps_*` the wheel notches; `flags`
    /// the axis stops (bits 0, 1) or the capabilities (keyboard, pointer,
    /// touch as bits 0, 1, 2); `output` the `OutputId` of absolute motion.
    #[derive(Debug, Clone)]
    struct InputEventRaw {
        kind: InputEventKind,
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
        output: u64,
        text: String,
    }

    unsafe extern "C++" {
        include!("luminaria/luminaria_rs.h");

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
        fn next_event(self: &DisplayShim, out: &mut EventRaw, input: &mut InputEventRaw) -> u32;
        fn take_output(self: &DisplayShim) -> UniquePtr<OutputShim>;
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
        fn capabilities(self: &WaylandBackendShim) -> u32;
        fn follow_seat(self: &WaylandBackendShim, seat: &SeatShim, touch: bool);

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
        fn surface_tree(surface_id: u64) -> Vec<SurfaceAtRaw>;
        fn surface_set_preferred_buffer_scale(surface_id: u64, scale: i32);
        fn surface_set_preferred_buffer_transform(surface_id: u64, transform: u32);

        // Core Protocols
        type CompositorShim;
        fn compositor_new(display: &DisplayShim, error: &mut String) -> UniquePtr<CompositorShim>;

        type SubcompositorShim;
        fn subcompositor_new(display: &DisplayShim, error: &mut String) -> UniquePtr<SubcompositorShim>;

        type XdgShellShim;
        fn xdg_shell_new(display: &DisplayShim, error: &mut String) -> UniquePtr<XdgShellShim>;
        fn set_bounds(self: &XdgShellShim, width: i32, height: i32);
        fn focus(self: &XdgShellShim, seat: &SeatShim, surface_id: u64);
        fn forward_window_requests(self: &XdgShellShim);
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
        fn release_keys(self: &SeatShim);
        fn pointer_enter(self: &SeatShim, surface_id: u64, sx: f64, sy: f64);
        fn pointer_clear_focus(self: &SeatShim);
        fn pointer_focus(self: &SeatShim) -> u64;
        fn pointer_grab(self: &SeatShim) -> u64;
        fn pointer_motion(self: &SeatShim, sx: f64, sy: f64);
        fn pointer_button(self: &SeatShim, button: u32, pressed: bool);
        fn pointer_axis(self: &SeatShim, dx: f64, dy: f64);
        fn pointer_axis_discrete(self: &SeatShim, dx_steps: i32, dy_steps: i32);
        fn pointer_axis_stop(self: &SeatShim, horizontal: bool, vertical: bool);
        #[allow(clippy::too_many_arguments)]
        fn pointer_axis_event(
            self: &SeatShim,
            dx: f64,
            dy: f64,
            dx_steps: i32,
            dy_steps: i32,
            stop_horizontal: bool,
            stop_vertical: bool,
        );
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

        type LayerShellShim;
        fn layer_shell_new(display: &DisplayShim, error: &mut String) -> UniquePtr<LayerShellShim>;
        fn layer_surfaces(self: &LayerShellShim) -> Vec<u64>;
        fn info(self: &LayerShellShim, surface_id: u64, out: &mut LayerSurfaceInfoRaw) -> bool;
        fn scope(self: &LayerShellShim, surface_id: u64) -> String;
        fn on_output(self: &LayerShellShim, surface_id: u64, output: &OutputGlobalShim) -> bool;
        fn configure(self: &LayerShellShim, surface_id: u64, width: i32, height: i32) -> u32;
        fn close(self: &LayerShellShim, surface_id: u64);
        fn arrange(self: &LayerShellShim, surface_id: u64, full: Rect, usable: &mut Rect) -> Rect;

        type OutputGlobalShim;
        fn output_global_new(display: &DisplayShim, width: i32, height: i32, name: &str, error: &mut String) -> UniquePtr<OutputGlobalShim>;
        fn output_global_for(
            display: &DisplayShim,
            output: &OutputShim,
            layout: &OutputLayoutShim,
            name: &str,
            error: &mut String,
        ) -> UniquePtr<OutputGlobalShim>;
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
        fn set_scale(self: &FractionalScaleManagerShim, surface_id: u64, scale: f64);

        type XdgActivationShim;
        fn xdg_activation_new(display: &DisplayShim, error: &mut String) -> UniquePtr<XdgActivationShim>;
        fn set_grant_tokens(self: &XdgActivationShim, grant: bool);

        type XdgDecorationManagerShim;
        fn xdg_decoration_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<XdgDecorationManagerShim>;
        fn set_default_mode(self: &XdgDecorationManagerShim, mode: u32);
        fn default_mode(self: &XdgDecorationManagerShim) -> u32;

        type CursorShapeManagerShim;
        fn cursor_shape_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<CursorShapeManagerShim>;

        type IdleInhibitManagerShim;
        fn idle_inhibit_manager_new(display: &DisplayShim, error: &mut String) -> UniquePtr<IdleInhibitManagerShim>;
        fn inhibited(self: &IdleInhibitManagerShim) -> bool;
        fn inhibitor_count(self: &IdleInhibitManagerShim) -> usize;
        fn inhibitor_surface(self: &IdleInhibitManagerShim, inhibitor: u64) -> u64;
        fn inhibitor_visible(self: &IdleInhibitManagerShim, inhibitor: u64) -> bool;
        fn set_inhibitor_visible(self: &IdleInhibitManagerShim, inhibitor: u64, visible: bool);

        type IdleNotifierShim;
        fn idle_notifier_new(display: &DisplayShim, error: &mut String) -> UniquePtr<IdleNotifierShim>;
        fn notify_activity(self: &IdleNotifierShim);
        fn set_inhibited(self: &IdleNotifierShim, inhibited: bool);
        fn inhibited(self: &IdleNotifierShim) -> bool;
        fn notification_count(self: &IdleNotifierShim) -> usize;
        fn idled_count(self: &IdleNotifierShim) -> usize;
        fn notification_info(self: &IdleNotifierShim, notification: u64, out: &mut IdleNotificationInfoRaw) -> bool;

        type TextInputManagerShim;
        fn text_input_manager_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<TextInputManagerShim>;
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

        type DataDeviceManagerShim;
        fn data_device_manager_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<DataDeviceManagerShim>;
        fn selection_mime_types(self: &DataDeviceManagerShim) -> Vec<String>;
        fn selection_receive(self: &DataDeviceManagerShim, mime: &str, fd: i32) -> bool;

        type PrimarySelectionManagerShim;
        fn primary_selection_manager_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<PrimarySelectionManagerShim>;
        fn selection_mime_types(self: &PrimarySelectionManagerShim) -> Vec<String>;
        fn selection_receive(self: &PrimarySelectionManagerShim, mime: &str, fd: i32) -> bool;

        type RelativePointerManagerShim;
        fn relative_pointer_manager_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<RelativePointerManagerShim>;

        type PointerConstraintsShim;
        fn pointer_constraints_new(display: &DisplayShim, seat: &SeatShim, error: &mut String) -> UniquePtr<PointerConstraintsShim>;
        fn constraint_info(self: &PointerConstraintsShim, surface_id: u64, out: &mut PointerConstraintInfoRaw) -> bool;
        fn constraint_region(self: &PointerConstraintsShim, surface_id: u64) -> Vec<Rect>;
        fn activate(self: &PointerConstraintsShim, surface_id: u64);
        fn deactivate(self: &PointerConstraintsShim, surface_id: u64);
        fn active_constraint(self: &PointerConstraintsShim) -> u64;

        // Shell primitives that are core, not GPU: the output layout, the scene
        // hit-test, the CPU compositor, popups and layer surfaces.
        type OutputLayoutShim;
        fn output_layout_new() -> UniquePtr<OutputLayoutShim>;
        fn add(self: &OutputLayoutShim, output: Pin<&mut OutputShim>, x: i32, y: i32);
        fn add_auto(self: &OutputLayoutShim, output: Pin<&mut OutputShim>);
        fn bounds(self: &OutputLayoutShim) -> Rect;
        fn remove(self: &OutputLayoutShim, output: Pin<&mut OutputShim>);
        fn output_box(self: &OutputLayoutShim, output: &OutputShim) -> Rect;
        fn primary_box(self: &OutputLayoutShim) -> Rect;

        fn raw_scene_hit_test(items: &[SceneItemRaw], x: f64, y: f64) -> SceneHitRaw;
        fn raw_scene_route_pointer(seat: &SeatShim, items: &[SceneItemRaw], x: f64, y: f64) -> SceneHitRaw;
        fn raw_scene_surface_origin(items: &[SceneItemRaw], surface_id: u64, x: &mut f64, y: &mut f64) -> bool;

        type CpuCompositorShim;
        fn cpu_compositor_new() -> UniquePtr<CpuCompositorShim>;
        fn composite_rect(self: Pin<&mut CpuCompositorShim>, x: i32, y: i32, width: i32, height: i32, r: f32, g: f32, b: f32, a: f32);
        fn finish(self: Pin<&mut CpuCompositorShim>, width: i32, height: i32, bg_r: f32, bg_g: f32, bg_b: f32, bg_a: f32) -> Vec<u8>;

        type PopupTreeShim;
        fn popup_tree_new(shell: &XdgShellShim) -> UniquePtr<PopupTreeShim>;
        fn set_parent(self: &PopupTreeShim, shell: &XdgShellShim, popup: u64, parent: u64) -> bool;
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
