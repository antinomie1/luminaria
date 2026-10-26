//! Desktop shell protocols: taskbars, workspaces, screen locking, output configuration, and input methods.

use std::pin::Pin;

use crate::core::{Display, Error, Holds};
use crate::ffi;
use crate::gpu::VulkanRenderer;
use crate::protocol::{DataDeviceManager, Seat, TextInputManager, XdgShell};
use crate::shell::OutputLayout;

/// Taskbar and window switcher protocol manager (`zwlr_foreign_toplevel_management_v1`).
pub struct ForeignToplevelManager<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::ForeignToplevelManagerShim>,
    _display: Holds<'d>,
}

impl<'d> ForeignToplevelManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::foreign_toplevel_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    /// Mirror `shell`'s toplevels to taskbars. The manager keeps listening
    /// to the shell, so the shell stays borrowed for as long as the manager
    /// lives. (`&mut self` is what pins `'d` here: through a shared reference
    /// the manager's lifetime could shrink to this call.)
    pub fn track(&mut self, shell: &'d XdgShell<'_>) {
        ffi::foreign_toplevel_track(self.raw.pin_mut(), &shell.raw);
    }
}

/// Workspace protocol manager (`ext_workspace_manager_v1`).
pub struct WorkspaceManager<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::WorkspaceManagerShim>,
    _display: Holds<'d>,
}

impl<'d> WorkspaceManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::workspace_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
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
pub struct SessionLockManager<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::SessionLockManagerShim>,
    _display: Holds<'d>,
}

impl<'d> SessionLockManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::session_lock_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    pub fn is_locked(&mut self) -> bool {
        ffi::session_lock_manager_locked(self.raw.pin_mut())
    }
}

/// Output layout configuration manager (`zwlr_output_manager_v1` for
/// kanshi/wlr-randr). Describes heads from the [`OutputLayout`] it borrows.
pub struct OutputManager<'a> {
    pub(crate) raw: cxx::UniquePtr<ffi::OutputManagerShim>,
    _borrows: Holds<'a>,
}

impl<'a> OutputManager<'a> {
    pub fn new(display: &'a Display, layout: &'a OutputLayout) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::output_manager_new(&display.raw, &layout.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _borrows: Holds::new() })
        }
    }

    pub fn update(&mut self) {
        self.raw.pin_mut().update();
    }
}

/// Clipboard manager protocol (`zwlr_data_control_manager_v1` for
/// wl-clipboard). Bridged into the [`DataDeviceManager`] it borrows.
pub struct DataControlManager<'a> {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::DataControlManagerShim>,
    _borrows: Holds<'a>,
}

impl<'a> DataControlManager<'a> {
    pub fn new(display: &'a Display, ddm: &'a DataDeviceManager<'_>) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::data_control_manager_new(&display.raw, &ddm.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _borrows: Holds::new() })
        }
    }
}

/// Input method manager (`zwp_input_method_manager_v2` for IBus/Fcitx).
/// Borrows the [`Seat`] and the [`TextInputManager`] it is bridged to.
pub struct InputMethodManager<'a> {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::InputMethodManagerShim>,
    _borrows: Holds<'a>,
}

impl<'a> InputMethodManager<'a> {
    pub fn new(
        display: &'a Display,
        seat: &'a Seat<'_>,
        tim: &'a TextInputManager<'_>,
    ) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::input_method_manager_new(&display.raw, &seat.raw, &tim.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _borrows: Holds::new() })
        }
    }
}

/// Convenience manager creating the complete set of standard Wayland desktop
/// globals. Borrows the [`Display`], and the renderer its linux-dmabuf uses.
pub struct DesktopGlobals<'a> {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::DesktopGlobalsShim>,
    _borrows: Holds<'a>,
}

impl<'a> DesktopGlobals<'a> {
    pub fn new(
        display: &'a Display,
        renderer: Option<&'a VulkanRenderer>,
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
            Ok(Self { raw, _borrows: Holds::new() })
        }
    }
}
