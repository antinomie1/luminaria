//! Idle: clients that keep the screen awake (idle-inhibit) and clients told
//! when the user went idle (ext-idle-notify).

use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi::{self, EventKind};

use super::SurfaceId;

/// An idle inhibitor, named by the binding: a surface may hold several.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct IdleInhibitorId(pub u64);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IdleInhibitEvent {
    NewInhibitor { inhibitor: IdleInhibitorId, surface: SurfaceId },
    InhibitorDestroy(IdleInhibitorId),
    /// Whether any visible inhibitor now keeps the session awake.
    Changed(bool),
}

/// The `zwp_idle_inhibit_manager_v1` global. An inhibitor counts only while
/// the compositor says its surface is visible.
pub struct IdleInhibitManager<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::IdleInhibitManagerShim>,
    _display: Holds<'d>,
}

impl<'d> IdleInhibitManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::idle_inhibit_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }
}

impl IdleInhibitManager<'_> {

    pub fn is_inhibited(&self) -> bool {
        self.raw.inhibited()
    }

    pub fn inhibitor_count(&self) -> usize {
        self.raw.inhibitor_count()
    }

    /// The surface `inhibitor` was created on, while it lives.
    pub fn inhibitor_surface(&self, inhibitor: IdleInhibitorId) -> Option<SurfaceId> {
        SurfaceId::from_raw(self.raw.inhibitor_surface(inhibitor.0))
    }

    pub fn is_visible(&self, inhibitor: IdleInhibitorId) -> bool {
        self.raw.inhibitor_visible(inhibitor.0)
    }

    /// Whether the inhibitor's surface is on screen; only visible ones inhibit.
    pub fn set_visible(&self, inhibitor: IdleInhibitorId, visible: bool) {
        self.raw.set_inhibitor_visible(inhibitor.0, visible);
    }
}

/// An idle notification, named by the binding.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct IdleNotificationId(pub u64);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IdleNotifierEvent {
    NewNotification(IdleNotificationId),
    NotificationDestroy(IdleNotificationId),
    /// Its timeout ran out (`idled`), or activity came back.
    StateChange { notification: IdleNotificationId, idled: bool },
}

/// What a client asked to be told about.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct IdleNotificationInfo {
    pub timeout_ms: u32,
    pub idled: bool,
    /// Whether idle inhibitors are ignored for this one.
    pub input_only: bool,
}

/// The `ext_idle_notifier_v1` global. Its timers run on the display's loop;
/// tell it about input with [`notify_activity`](Self::notify_activity).
pub struct IdleNotifier<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::IdleNotifierShim>,
    _display: Holds<'d>,
}

impl<'d> IdleNotifier<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::idle_notifier_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }
}

impl IdleNotifier<'_> {

    /// The user did something: every notification resumes and restarts.
    pub fn notify_activity(&self) {
        self.raw.notify_activity();
    }

    /// Feed it [`IdleInhibitManager::is_inhibited`]: while inhibited, only
    /// input-only notifications go idle.
    pub fn set_inhibited(&self, inhibited: bool) {
        self.raw.set_inhibited(inhibited);
    }

    pub fn is_inhibited(&self) -> bool {
        self.raw.inhibited()
    }

    pub fn notification_count(&self) -> usize {
        self.raw.notification_count()
    }

    pub fn idled_count(&self) -> usize {
        self.raw.idled_count()
    }

    pub fn notification(&self, notification: IdleNotificationId) -> Option<IdleNotificationInfo> {
        let mut info = ffi::IdleNotificationInfoRaw::default();
        self.raw.notification_info(notification.0, &mut info).then_some(IdleNotificationInfo {
            timeout_ms: info.timeout_ms,
            idled: info.idled,
            input_only: info.input_only,
        })
    }
}

impl IdleInhibitEvent {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<IdleInhibitEvent> {
        return Some(match raw.kind {
            EventKind::NewIdleInhibitor => IdleInhibitEvent::NewInhibitor {
                inhibitor: IdleInhibitorId(raw.id),
                surface: SurfaceId(raw.other),
            },
            EventKind::IdleInhibitorDestroy => IdleInhibitEvent::InhibitorDestroy(IdleInhibitorId(raw.id)),
            EventKind::IdleInhibitChange => IdleInhibitEvent::Changed(raw.flag),
            _ => return None,
        });
        None
    }
}

impl IdleNotifierEvent {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<IdleNotifierEvent> {
        let notification = IdleNotificationId(raw.id);
        return Some(match raw.kind {
            EventKind::NewIdleNotification => IdleNotifierEvent::NewNotification(notification),
            EventKind::IdleNotificationDestroy => IdleNotifierEvent::NotificationDestroy(notification),
            EventKind::IdleStateChange => IdleNotifierEvent::StateChange { notification, idled: raw.flag },
            _ => return None,
        });
        None
    }
}
