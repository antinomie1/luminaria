//! Desktop shell protocols: taskbars, workspaces, screen locking, output configuration, and input methods.

use std::pin::Pin;

use crate::core::{Display, Error};
use crate::ffi;
use crate::gpu::VulkanRenderer;
use crate::protocol::{DataDeviceManager, Seat, TextInputManager, XdgShell};
use crate::shell::OutputLayout;

/// Taskbar and window switcher protocol manager (`zwlr_foreign_toplevel_management_v1`).
pub struct ForeignToplevelManager {
    pub(crate) raw: cxx::UniquePtr<ffi::ForeignToplevelManagerShim>,
}

impl ForeignToplevelManager {
    pub fn new(display: &Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::foreign_toplevel_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn track(&mut self, shell: &mut XdgShell) {
        let mut ftm = self.raw.pin_mut();
        let mut sh = shell.raw.pin_mut();
        ffi::foreign_toplevel_track(ftm.as_mut(), sh.as_mut());
    }
}

/// Workspace protocol manager (`ext_workspace_manager_v1`).
pub struct WorkspaceManager {
    pub(crate) raw: cxx::UniquePtr<ffi::WorkspaceManagerShim>,
}

impl WorkspaceManager {
    pub fn new(display: &Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::workspace_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    fn pin(&mut self) -> Pin<&mut ffi::WorkspaceManagerShim> {
        self.raw.pin_mut()
    }

    pub fn create_group(&mut self) -> u32 {
        ffi::workspace_manager_create_group(self.pin())
    }

    pub fn create_workspace(&mut self, group: u32, name: &str) -> u32 {
        ffi::workspace_manager_create_workspace(self.pin(), group, name)
    }

    pub fn remove_workspace(&mut self, workspace: u32) {
        ffi::workspace_manager_remove_workspace(self.pin(), workspace);
    }

    pub fn activate_workspace(&mut self, workspace: u32) {
        ffi::workspace_manager_activate_workspace(self.pin(), workspace);
    }
}

/// Screen locking protocol manager (`ext_session_lock_manager_v1`).
pub struct SessionLockManager {
    pub(crate) raw: cxx::UniquePtr<ffi::SessionLockManagerShim>,
}

impl SessionLockManager {
    pub fn new(display: &Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::session_lock_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn is_locked(&mut self) -> bool {
        ffi::session_lock_manager_locked(self.raw.pin_mut())
    }
}

/// Output layout configuration manager (`zwlr_output_manager_v1` for kanshi/wlr-randr).
pub struct OutputManager {
    pub(crate) raw: cxx::UniquePtr<ffi::OutputManagerShim>,
}

impl OutputManager {
    pub fn new(display: &Display, layout: &OutputLayout) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::output_manager_new(&display.raw, &layout.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn update(&mut self) {
        self.raw.pin_mut().update();
    }
}

/// Clipboard manager protocol (`zwlr_data_control_manager_v1` for wl-clipboard).
pub struct DataControlManager {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::DataControlManagerShim>,
}

impl DataControlManager {
    pub fn new(display: &Display, ddm: &DataDeviceManager) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::data_control_manager_new(&display.raw, &ddm.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

/// Input method manager (`zwp_input_method_manager_v2` for IBus/Fcitx).
pub struct InputMethodManager {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::InputMethodManagerShim>,
}

impl InputMethodManager {
    pub fn new(
        display: &Display,
        seat: &Seat,
        tim: &TextInputManager,
    ) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::input_method_manager_new(&display.raw, &seat.raw, &tim.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

/// Convenience manager creating the complete set of standard Wayland desktop globals.
pub struct DesktopGlobals {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::DesktopGlobalsShim>,
}

impl DesktopGlobals {
    pub fn new(
        display: &Display,
        renderer: Option<&VulkanRenderer>,
        seat_name: &str,
    ) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = if let Some(r) = renderer {
            ffi::desktop_globals_new(&display.raw, &r.raw, seat_name, &mut err)
        } else {
            ffi::desktop_globals_new_headless(&display.raw, seat_name, &mut err)
        };
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}
