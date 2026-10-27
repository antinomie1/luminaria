//! The desktop bridge: `import luminaria.desktop;` on the C++ side.
//!
//! Cargo feature `desktop`. The privileged desktop-shell protocols — taskbar
//! window lists, workspaces, screen locking, clipboard managers, input methods,
//! output configuration — which ADR 0003 keeps out of the core module because
//! each of them lets one client enumerate or drive another client's objects.
//!
//! It needs nothing of `luminaria.gpu`: a nested or headless desktop shell
//! pays for no Vulkan.

#[cxx::bridge(namespace = "lum")]
pub(crate) mod ffi {
    #[derive(Debug, Clone, Copy, Default)]
    struct LockSurfaceInfoRaw {
        lock: u64,
        mapped: bool,
        width: u32,
        height: u32,
        has_output: bool,
    }

    unsafe extern "C++" {
        include!("luminaria/luminaria_rs.h");
        include!("luminaria/luminaria_rs_desktop.h");
        // Shared structs (EventRaw, Rect) are defined by the core bridge.
        include!("luminaria/src/ffi.rs.h");

        // Core and GPU types this bridge's signatures mention.
        type DisplayShim = crate::ffi::DisplayShim;
        type EventRaw = crate::ffi::EventRaw;
        type Rect = crate::ffi::Rect;
        type SeatShim = crate::ffi::SeatShim;
        type XdgShellShim = crate::ffi::XdgShellShim;
        type TextInputManagerShim = crate::ffi::TextInputManagerShim;
        type DataDeviceManagerShim = crate::ffi::DataDeviceManagerShim;
        type OutputGlobalShim = crate::ffi::OutputGlobalShim;
        type OutputLayoutShim = crate::ffi::OutputLayoutShim;

        type ForeignToplevelManagerShim;
        fn foreign_toplevel_manager_new(
            display: &DisplayShim,
            error: &mut String,
        ) -> UniquePtr<ForeignToplevelManagerShim>;
        fn foreign_toplevel_track(ftm: Pin<&mut ForeignToplevelManagerShim>, shell: &XdgShellShim);

        type WorkspaceManagerShim;
        fn workspace_manager_new(
            display: &DisplayShim,
            error: &mut String,
        ) -> UniquePtr<WorkspaceManagerShim>;
        fn create_group(self: &WorkspaceManagerShim) -> u32;
        fn create_workspace(self: &WorkspaceManagerShim, group: u32, name: &str) -> u32;
        fn remove_workspace(self: &WorkspaceManagerShim, workspace: u32);
        fn activate_workspace(self: &WorkspaceManagerShim, workspace: u32);

        type SessionLockManagerShim;
        fn session_lock_manager_new(
            display: &DisplayShim,
            error: &mut String,
        ) -> UniquePtr<SessionLockManagerShim>;
        fn locked(self: &SessionLockManagerShim) -> bool;
        fn current(self: &SessionLockManagerShim) -> u64;
        fn lock_locked(self: &SessionLockManagerShim, lock: u64) -> bool;
        fn send_locked(self: &SessionLockManagerShim, lock: u64);
        fn send_finished(self: &SessionLockManagerShim, lock: u64);
        fn lock_surfaces(self: &SessionLockManagerShim, lock: u64) -> Vec<u64>;
        fn lock_surface_info(
            self: &SessionLockManagerShim,
            surface_id: u64,
            out: &mut LockSurfaceInfoRaw,
        ) -> bool;
        fn lock_surface_configure(
            self: &SessionLockManagerShim,
            surface_id: u64,
            width: u32,
            height: u32,
        ) -> u32;
        fn lock_surface_on_output(
            self: &SessionLockManagerShim,
            surface_id: u64,
            output: &OutputGlobalShim,
        ) -> bool;

        type OutputManagerShim;
        fn output_manager_new(
            display: &DisplayShim,
            layout: &OutputLayoutShim,
            error: &mut String,
        ) -> UniquePtr<OutputManagerShim>;
        fn update(self: Pin<&mut OutputManagerShim>);
        fn set_auto_verdict(self: &OutputManagerShim, succeed: bool);

        type DataControlManagerShim;
        fn data_control_manager_new(
            display: &DisplayShim,
            ddm: &DataDeviceManagerShim,
            error: &mut String,
        ) -> UniquePtr<DataControlManagerShim>;

        type InputMethodManagerShim;
        fn input_method_manager_new(
            display: &DisplayShim,
            seat: &SeatShim,
            tim: &TextInputManagerShim,
            error: &mut String,
        ) -> UniquePtr<InputMethodManagerShim>;
        fn current(self: &InputMethodManagerShim) -> u64;
        fn active(self: &InputMethodManagerShim, input_method: u64) -> bool;
        fn has_keyboard_grab(self: &InputMethodManagerShim, input_method: u64) -> bool;
        fn grab_send_key(
            self: &InputMethodManagerShim,
            input_method: u64,
            time: u32,
            key: u32,
            pressed: bool,
        );
        fn grab_send_modifiers(
            self: &InputMethodManagerShim,
            input_method: u64,
            depressed: u32,
            latched: u32,
            locked: u32,
            group: u32,
        );
        fn grab_send_repeat_info(
            self: &InputMethodManagerShim,
            input_method: u64,
            rate: i32,
            delay: i32,
        );
        fn popup_surfaces(self: &InputMethodManagerShim, input_method: u64) -> Vec<u64>;
        fn popup_text_input_rectangle(self: &InputMethodManagerShim, surface_id: u64) -> Rect;

        type GammaControlManagerShim;
        fn gamma_control_manager_new(
            display: &DisplayShim,
            gamma_size: u32,
            error: &mut String,
        ) -> UniquePtr<GammaControlManagerShim>;
        fn gamma_size(self: &GammaControlManagerShim) -> u32;
        fn get_ramp(
            self: &GammaControlManagerShim,
            output_res_id: u64,
            red: &mut Vec<u16>,
            green: &mut Vec<u16>,
            blue: &mut Vec<u16>,
        ) -> bool;

        type CtmControlManagerShim;
        fn ctm_control_manager_new(
            display: &DisplayShim,
            error: &mut String,
        ) -> UniquePtr<CtmControlManagerShim>;
        fn get_ctm(
            self: &CtmControlManagerShim,
            output_res_id: u64,
            out_matrix_9: &mut Vec<f64>,
        ) -> bool;

        type VirtualKeyboardManagerShim;
        fn virtual_keyboard_manager_new(
            display: &DisplayShim,
            error: &mut String,
        ) -> UniquePtr<VirtualKeyboardManagerShim>;

        type VirtualPointerManagerShim;
        fn virtual_pointer_manager_new(
            display: &DisplayShim,
            error: &mut String,
        ) -> UniquePtr<VirtualPointerManagerShim>;

    }
}
