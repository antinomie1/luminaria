//! Desktop shell protocols: taskbars, workspaces, screen locking, output configuration, and input methods.

use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi::EventKind;
use crate::ffi_desktop as ffi;
use crate::protocol::{
    DataDeviceManager, OutputGlobal, Seat, SurfaceId, TextInputManager, XdgShell,
};
use crate::shell::OutputLayout;
use crate::util::Rect;

/// Taskbar and window switcher request kinds.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ForeignToplevelRequestKind {
    Maximize,
    Unmaximize,
    Minimize,
    Unminimize,
    Fullscreen,
    Unfullscreen,
    Activate,
    Close,
}

impl ForeignToplevelRequestKind {
    pub(crate) fn from_u32(val: u32) -> Option<Self> {
        match val {
            0 => Some(Self::Maximize),
            1 => Some(Self::Unmaximize),
            2 => Some(Self::Minimize),
            3 => Some(Self::Unminimize),
            4 => Some(Self::Fullscreen),
            5 => Some(Self::Unfullscreen),
            6 => Some(Self::Activate),
            7 => Some(Self::Close),
            _ => None,
        }
    }
}

/// A request from a window switcher or taskbar (`zwlr_foreign_toplevel_handle_v1`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ForeignToplevelRequest {
    pub surface: SurfaceId,
    pub kind: ForeignToplevelRequestKind,
}

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
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                raw,
                _display: Holds::new(),
            })
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

/// The `ext_foreign_toplevel_list_v1` global: every mapped window with a title,
/// an app id and a stable opaque identifier, and nothing a client could do to
/// one — the read-only list a screen-sharing portal picks a window from. Pair it
/// with [`ScreencopyManager::enable_toplevel_capture`](crate::render::ScreencopyManager::enable_toplevel_capture)
/// to capture the window picked.
pub struct ForeignToplevelList<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::ForeignToplevelListShim>,
    _display: Holds<'d>,
}

impl<'d> ForeignToplevelList<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::foreign_toplevel_list_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                raw,
                _display: Holds::new(),
            })
        }
    }

    /// Publish every window of `shell` and keep the list current by itself.
    /// The shell stays borrowed for as long as the list lives.
    pub fn track(&mut self, shell: &'d XdgShell<'_>) {
        ffi::foreign_toplevel_list_track(self.raw.pin_mut(), &shell.raw);
    }

    /// How many windows are announced right now.
    pub fn toplevel_count(&self) -> u32 {
        self.raw.toplevel_count()
    }
}

/// A workspace request kind from a pager or panel.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WorkspaceRequestKind {
    Activate,
    Deactivate,
    Remove,
    Assign,
    Create,
}

impl WorkspaceRequestKind {
    pub(crate) fn from_u32(val: u32) -> Option<Self> {
        match val {
            0 => Some(Self::Activate),
            1 => Some(Self::Deactivate),
            2 => Some(Self::Remove),
            3 => Some(Self::Assign),
            4 => Some(Self::Create),
            _ => None,
        }
    }
}

/// A workspace request (`ext_workspace_manager_v1` or `ext_workspace_handle_v1`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkspaceRequest {
    pub kind: WorkspaceRequestKind,
    pub workspace: u32,
    pub group: u32,
    pub name: String,
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
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                raw,
                _display: Holds::new(),
            })
        }
    }

    pub fn create_group(&mut self) -> u32 {
        self.raw.create_group()
    }

    pub fn create_workspace(&mut self, group: u32, name: &str) -> u32 {
        self.raw.create_workspace(group, name)
    }

    pub fn remove_workspace(&mut self, workspace: u32) {
        self.raw.remove_workspace(workspace);
    }

    pub fn activate_workspace(&mut self, workspace: u32) {
        self.raw.activate_workspace(workspace);
    }
}

/// A lock client's claim on the session, named by the binding.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct LockId(pub u64);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionLockEvent {
    /// A client wants the session locked. Cover every output, then
    /// [`SessionLock::send_locked`]; or refuse with [`SessionLock::send_finished`].
    NewLock(LockId),
    /// The lock client made `surface` its lock surface for one output.
    /// Configure it to that output's size.
    NewSurface {
        lock: LockId,
        surface: SurfaceId,
    },
    SurfaceMap(SurfaceId),
    SurfaceDestroy(SurfaceId),
    /// The lock object is gone. `unlocked`: the client unlocked in order.
    /// Otherwise it died holding the lock, and the session STAYS locked
    /// ([`SessionLockManager::is_locked`] says so) until a new lock client
    /// unlocks it.
    LockDestroy {
        lock: LockId,
        unlocked: bool,
    },
}

/// Screen locking protocol manager (`ext_session_lock_manager_v1`). It fails
/// closed: drawing and input should follow [`is_locked`](Self::is_locked), not
/// whether a lock client is alive.
pub struct SessionLockManager<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::SessionLockManagerShim>,
    _display: Holds<'d>,
}

impl<'d> SessionLockManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::session_lock_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                raw,
                _display: Holds::new(),
            })
        }
    }
}

impl SessionLockManager<'_> {
    /// True from the first `send_locked` until a client's orderly unlock.
    pub fn is_locked(&self) -> bool {
        self.raw.locked()
    }

    /// The lock currently held, if its client is alive. `None` does not mean
    /// unlocked: see [`is_locked`](Self::is_locked).
    pub fn current(&self) -> Option<SessionLock<'_>> {
        match self.raw.current() {
            0 => None,
            id => Some(self.lock(LockId(id))),
        }
    }

    /// A lock by id. Once its client is gone every call on it is a no-op.
    pub fn lock(&self, lock: LockId) -> SessionLock<'_> {
        SessionLock {
            mgr: &*self.raw,
            lock,
        }
    }

    /// The lock surface whose surface is `surface`, if there is one now.
    pub fn lock_surface(&self, surface: SurfaceId) -> Option<LockSurface<'_>> {
        let mut info = ffi::LockSurfaceInfoRaw::default();
        self.raw
            .lock_surface_info(surface.0, &mut info)
            .then_some(LockSurface {
                mgr: self.raw.as_ref()?,
                surface,
            })
    }
}

/// A lock, looked up on every call.
#[derive(Clone, Copy)]
pub struct SessionLock<'m> {
    mgr: &'m ffi::SessionLockManagerShim,
    lock: LockId,
}

impl SessionLock<'_> {
    pub fn id(&self) -> LockId {
        self.lock
    }

    /// "The screen is covered." Send it only once nothing else is visible.
    pub fn send_locked(&self) {
        self.mgr.send_locked(self.lock.0);
    }

    /// Refuse the lock, or give up one that can no longer be honoured.
    pub fn send_finished(&self) {
        self.mgr.send_finished(self.lock.0);
    }

    /// Whether `send_locked` has been sent.
    pub fn is_locked(&self) -> bool {
        self.mgr.lock_locked(self.lock.0)
    }

    pub fn surfaces(&self) -> Vec<SurfaceId> {
        self.mgr
            .lock_surfaces(self.lock.0)
            .into_iter()
            .map(SurfaceId)
            .collect()
    }
}

/// One output's lock surface, looked up by its surface on every call.
#[derive(Clone, Copy)]
pub struct LockSurface<'m> {
    mgr: &'m ffi::SessionLockManagerShim,
    surface: SurfaceId,
}

impl LockSurface<'_> {
    fn info(&self) -> ffi::LockSurfaceInfoRaw {
        let mut info = ffi::LockSurfaceInfoRaw::default();
        self.mgr.lock_surface_info(self.surface.0, &mut info);
        info
    }

    pub fn surface(&self) -> SurfaceId {
        self.surface
    }

    pub fn lock(&self) -> LockId {
        LockId(self.info().lock)
    }

    pub fn is_mapped(&self) -> bool {
        self.info().mapped
    }

    /// The size last configured.
    pub fn size(&self) -> (u32, u32) {
        let i = self.info();
        (i.width, i.height)
    }

    /// Whether this is the lock surface for `output`.
    pub fn is_on(&self, output: &OutputGlobal<'_>) -> bool {
        self.mgr.lock_surface_on_output(self.surface.0, &output.raw)
    }

    /// Size it to its output. Returns the configure serial.
    pub fn configure(&self, width: u32, height: u32) -> u32 {
        self.mgr
            .lock_surface_configure(self.surface.0, width, height)
    }
}

/// A client applied or tested a screen arrangement (`zwlr_output_configuration_v1`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OutputConfigRequest {
    pub test: bool,
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
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                raw,
                _borrows: Holds::new(),
            })
        }
    }

    pub fn update(&mut self) {
        self.raw.pin_mut().update();
    }

    pub fn set_auto_verdict(&self, succeed: bool) {
        self.raw.set_auto_verdict(succeed);
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
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                raw,
                _borrows: Holds::new(),
            })
        }
    }
}

/// An input method client (one per seat), named by the binding.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct InputMethodId(pub u64);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InputMethodEvent {
    New(InputMethodId),
    /// It took (or released) the keyboard: while grabbed, keys go to it
    /// through [`InputMethod::grab_send_key`] instead of the focused client.
    GrabChange {
        input_method: InputMethodId,
        grabbed: bool,
    },
    Destroy(InputMethodId),
    /// A candidate window. Draw it next to [`InputMethodManager::popup_text_input_rectangle`].
    NewPopupSurface {
        input_method: InputMethodId,
        surface: SurfaceId,
    },
    PopupSurfaceDestroy(SurfaceId),
}

/// Input method manager (`zwp_input_method_manager_v2` for IBus/Fcitx).
/// Borrows the [`Seat`] and the [`TextInputManager`] it is bridged to.
pub struct InputMethodManager<'a> {
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
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                raw,
                _borrows: Holds::new(),
            })
        }
    }
}

impl InputMethodManager<'_> {
    /// The input method bound to the seat.
    pub fn current(&self) -> Option<InputMethod<'_>> {
        match self.raw.current() {
            0 => None,
            id => Some(InputMethod {
                mgr: self.raw.as_ref()?,
                id: InputMethodId(id),
            }),
        }
    }

    pub fn input_method(&self, id: InputMethodId) -> InputMethod<'_> {
        InputMethod {
            mgr: &*self.raw,
            id,
        }
    }

    /// The text cursor a candidate window belongs next to, in the coordinates
    /// of the text field's surface.
    pub fn popup_text_input_rectangle(&self, popup: SurfaceId) -> Rect {
        self.raw.popup_text_input_rectangle(popup.0)
    }
}

/// An input method, looked up on every call; once it is gone, every call is a
/// no-op.
#[derive(Clone, Copy)]
pub struct InputMethod<'m> {
    mgr: &'m ffi::InputMethodManagerShim,
    id: InputMethodId,
}

impl InputMethod<'_> {
    pub fn id(&self) -> InputMethodId {
        self.id
    }

    /// Whether a text input is enabled and focused for it to serve.
    pub fn is_active(&self) -> bool {
        self.mgr.active(self.id.0)
    }

    pub fn has_keyboard_grab(&self) -> bool {
        self.mgr.has_keyboard_grab(self.id.0)
    }

    pub fn grab_send_key(&self, time: u32, key: u32, pressed: bool) {
        self.mgr.grab_send_key(self.id.0, time, key, pressed);
    }

    pub fn grab_send_modifiers(&self, depressed: u32, latched: u32, locked: u32, group: u32) {
        self.mgr
            .grab_send_modifiers(self.id.0, depressed, latched, locked, group);
    }

    pub fn grab_send_repeat_info(&self, rate: i32, delay: i32) {
        self.mgr.grab_send_repeat_info(self.id.0, rate, delay);
    }

    pub fn popup_surfaces(&self) -> Vec<SurfaceId> {
        self.mgr
            .popup_surfaces(self.id.0)
            .into_iter()
            .map(SurfaceId)
            .collect()
    }
}

impl ForeignToplevelRequest {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<ForeignToplevelRequest> {
        match raw.kind {
            EventKind::ForeignToplevelRequest => {
                let surface = SurfaceId(raw.id);
                if let Some(kind) = ForeignToplevelRequestKind::from_u32(raw.value) {
                    return Some(ForeignToplevelRequest { surface, kind });
                }
            }
            _ => {}
        }
        None
    }
}

impl WorkspaceRequest {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<WorkspaceRequest> {
        match raw.kind {
            EventKind::WorkspaceRequest => {
                if let Some(kind) = WorkspaceRequestKind::from_u32(raw.value) {
                    return Some(WorkspaceRequest {
                        kind,
                        workspace: raw.id as u32,
                        group: raw.other as u32,
                        name: raw.text,
                    });
                }
            }
            _ => {}
        }
        None
    }
}

impl SessionLockEvent {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<SessionLockEvent> {
        return Some(match raw.kind {
            EventKind::NewSessionLock => SessionLockEvent::NewLock(LockId(raw.id)),
            EventKind::NewLockSurface => SessionLockEvent::NewSurface {
                lock: LockId(raw.id),
                surface: SurfaceId(raw.other),
            },
            EventKind::LockSurfaceMap => SessionLockEvent::SurfaceMap(SurfaceId(raw.id)),
            EventKind::LockSurfaceDestroy => SessionLockEvent::SurfaceDestroy(SurfaceId(raw.id)),
            EventKind::SessionLockDestroy => SessionLockEvent::LockDestroy {
                lock: LockId(raw.id),
                unlocked: raw.flag,
            },
            _ => return None,
        });
        None
    }
}

impl OutputConfigRequest {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<OutputConfigRequest> {
        match raw.kind {
            EventKind::OutputConfigRequest => {
                return Some(OutputConfigRequest { test: raw.flag });
            }
            _ => {}
        }
        None
    }
}

impl InputMethodEvent {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<InputMethodEvent> {
        let input_method = InputMethodId(raw.id);
        return Some(match raw.kind {
            EventKind::NewInputMethod => InputMethodEvent::New(input_method),
            EventKind::InputMethodGrabChange => InputMethodEvent::GrabChange {
                input_method,
                grabbed: raw.flag,
            },
            EventKind::InputMethodDestroy => InputMethodEvent::Destroy(input_method),
            EventKind::NewInputPopupSurface => InputMethodEvent::NewPopupSurface {
                input_method,
                surface: SurfaceId(raw.other),
            },
            EventKind::InputPopupSurfaceDestroy => {
                InputMethodEvent::PopupSurfaceDestroy(SurfaceId(raw.id))
            }
            _ => return None,
        });
        None
    }
}

/// 16-bit RGB gamma table.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GammaRamp {
    pub red: Vec<u16>,
    pub green: Vec<u16>,
    pub blue: Vec<u16>,
}

/// A gamma change event from a client (`zwlr_gamma_control_v1`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GammaEvent {
    pub output_resource: u64,
    pub has_ramp: bool,
}

impl GammaEvent {
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<GammaEvent> {
        match raw.kind {
            EventKind::GammaChange => Some(GammaEvent {
                output_resource: raw.id,
                has_ramp: raw.flag,
            }),
            _ => None,
        }
    }
}

/// Output gamma ramp control manager (`zwlr_gamma_control_manager_v1`).
pub struct GammaControlManager<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::GammaControlManagerShim>,
    _display: Holds<'d>,
}

impl<'d> GammaControlManager<'d> {
    pub fn new(display: &'d Display, gamma_size: u32) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::gamma_control_manager_new(&display.raw, gamma_size, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                raw,
                _display: Holds::new(),
            })
        }
    }

    pub fn gamma_size(&self) -> u32 {
        self.raw.gamma_size()
    }

    pub fn ramp(&self, output_resource: u64) -> Option<GammaRamp> {
        let mut red = Vec::new();
        let mut green = Vec::new();
        let mut blue = Vec::new();
        if self
            .raw
            .get_ramp(output_resource, &mut red, &mut green, &mut blue)
        {
            Some(GammaRamp { red, green, blue })
        } else {
            None
        }
    }
}

/// A Color Transform Matrix (CTM) update event (`hyprland_ctm_control_v1`).
#[derive(Debug, Clone, PartialEq)]
pub struct CtmEvent {
    pub output_resource: u64,
}

impl CtmEvent {
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<CtmEvent> {
        match raw.kind {
            EventKind::CtmChange => Some(CtmEvent {
                output_resource: raw.id,
            }),
            _ => None,
        }
    }
}

/// Output Color Transform Matrix manager (`hyprland_ctm_control_manager_v1`).
pub struct CtmControlManager<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::CtmControlManagerShim>,
    _display: Holds<'d>,
}

impl<'d> CtmControlManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::ctm_control_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                raw,
                _display: Holds::new(),
            })
        }
    }

    pub fn matrix(&self, output_resource: u64) -> Option<[f64; 9]> {
        let mut mat = Vec::new();
        if self.raw.get_ctm(output_resource, &mut mat) && mat.len() == 9 {
            let mut arr = [0.0; 9];
            arr.copy_from_slice(&mat);
            Some(arr)
        } else {
            None
        }
    }
}

/// Virtual keyboard key event (`zwp_virtual_keyboard_v1`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct VirtualKeyEvent {
    pub seat_resource: u64,
    pub time: u32,
    pub key: u32,
    pub pressed: bool,
}

impl VirtualKeyEvent {
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<VirtualKeyEvent> {
        match raw.kind {
            EventKind::VirtualKey => Some(VirtualKeyEvent {
                seat_resource: raw.id,
                time: raw.serial,
                key: raw.value,
                pressed: raw.flag,
            }),
            _ => None,
        }
    }
}

/// Virtual keyboard modifier and group event (`zwp_virtual_keyboard_v1`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct VirtualModifiersEvent {
    pub seat_resource: u64,
    pub depressed: u32,
    pub latched: u32,
    pub locked: u32,
    pub group: u32,
}

impl VirtualModifiersEvent {
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<VirtualModifiersEvent> {
        match raw.kind {
            EventKind::VirtualModifiers => Some(VirtualModifiersEvent {
                seat_resource: raw.id,
                depressed: raw.value,
                latched: raw.serial,
                locked: raw.x as u32,
                group: raw.y as u32,
            }),
            _ => None,
        }
    }
}

/// Virtual keyboard manager (`zwp_virtual_keyboard_manager_v1`).
pub struct VirtualKeyboardManager<'d> {
    pub(crate) _raw: cxx::UniquePtr<ffi::VirtualKeyboardManagerShim>,
    _display: Holds<'d>,
}

impl<'d> VirtualKeyboardManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::virtual_keyboard_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                _raw: raw,
                _display: Holds::new(),
            })
        }
    }
}

/// Virtual pointer event (`zwlr_virtual_pointer_v1`).
#[derive(Debug, Clone, PartialEq)]
pub enum VirtualPointerEvent {
    Motion {
        seat_resource: u64,
        time: u32,
        dx: f64,
        dy: f64,
    },
    MotionAbsolute {
        seat_resource: u64,
        output_resource: u64,
        time: u32,
        x: u32,
        y: u32,
        x_extent: u32,
        y_extent: u32,
    },
    Button {
        seat_resource: u64,
        time: u32,
        button: u32,
        pressed: bool,
    },
    Axis {
        seat_resource: u64,
        time: u32,
        axis: u32,
        value: f64,
    },
    Frame {
        seat_resource: u64,
    },
}

impl VirtualPointerEvent {
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<VirtualPointerEvent> {
        match raw.kind {
            EventKind::VirtualPointerMotion => Some(VirtualPointerEvent::Motion {
                seat_resource: raw.id,
                time: raw.serial,
                dx: raw.x as f64 / 256.0,
                dy: raw.y as f64 / 256.0,
            }),
            EventKind::VirtualPointerMotionAbsolute => {
                let y_extent = raw.text.parse::<u32>().unwrap_or(0);
                Some(VirtualPointerEvent::MotionAbsolute {
                    seat_resource: raw.id,
                    output_resource: raw.other,
                    time: raw.serial,
                    x: raw.x as u32,
                    y: raw.y as u32,
                    x_extent: raw.value,
                    y_extent,
                })
            }
            EventKind::VirtualPointerButton => Some(VirtualPointerEvent::Button {
                seat_resource: raw.id,
                time: raw.serial,
                button: raw.value,
                pressed: raw.flag,
            }),
            EventKind::VirtualPointerAxis => Some(VirtualPointerEvent::Axis {
                seat_resource: raw.id,
                time: raw.serial,
                axis: raw.value,
                value: raw.x as f64 / 256.0,
            }),
            EventKind::VirtualPointerFrame => Some(VirtualPointerEvent::Frame {
                seat_resource: raw.id,
            }),
            _ => None,
        }
    }
}

/// Virtual pointer manager (`zwlr_virtual_pointer_manager_v1`).
pub struct VirtualPointerManager<'d> {
    pub(crate) _raw: cxx::UniquePtr<ffi::VirtualPointerManagerShim>,
    _display: Holds<'d>,
}

impl<'d> VirtualPointerManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::virtual_pointer_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                _raw: raw,
                _display: Holds::new(),
            })
        }
    }
}
