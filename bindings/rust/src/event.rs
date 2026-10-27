//! One stream for everything luminaria reports.
//!
//! The library is driven by C++ signals, and C++ never calls Rust. Every
//! signal a wrapper forwards is queued, on the thread the [`Display`] lives on,
//! in the order the signals fired, and [`Display::dispatch`] hands the lot back
//! as [`Events`] once it returns:
//!
//! ```no_run
//! # use luminaria::{core::Display, Event, protocol::XdgEvent};
//! # fn run(display: &Display) -> Result<(), luminaria::core::Error> {
//! for event in display.dispatch(None)? {
//!     match event {
//!         Event::Xdg(XdgEvent::Toplevel(surface, e)) => { /* ... */ }
//!         Event::Output(output, e) => { /* frames, presentation, mode changes */ }
//!         _ => {}
//!     }
//! }
//! # Ok(()) }
//! ```
//!
//! There is one queue and not one per global, so nothing can grow because
//! nobody drained it: a dispatch collects every event of every wrapper, and
//! what the compositor does not match is simply dropped.
//!
//! Events arrive after the fact. Objects a client creates are therefore named,
//! never lent: a surface by its [`SurfaceId`], everything else by an id the
//! binding hands out and never reuses. Look the object up again when the event
//! is handled; one destroyed since answers with nothing.
//!
//! Each payload converts back out of an [`Event`] with `TryFrom`, which gives
//! back the event untouched when it is something else:
//!
//! ```no_run
//! # use luminaria::{Event, protocol::XdgEvent};
//! # fn f(events: Vec<Event>) {
//! let xdg: Vec<XdgEvent> = events.into_iter().filter_map(|e| e.try_into().ok()).collect();
//! # }
//! ```

use std::fmt;

use crate::backend::{BackendEvent, InputEvent, Output, OutputEvent, OutputId, PresentEvent};
use crate::core::Display;
#[cfg(feature = "desktop")]
use crate::desktop::{
    ForeignToplevelRequest, InputMethodEvent, OutputConfigRequest, SessionLockEvent, WorkspaceRequest,
};
use crate::ffi::{self, EventKind as K};
use crate::protocol::{
    ActivationEvent, CompositorEvent, CursorShapeRequest, DecorationRequest, IdleInhibitEvent,
    IdleNotifierEvent, LayerSurfaceEvent, PointerConstraintEvent, SeatEvent, SelectionChange, SurfaceId,
    TextInputEvent,
    ToplevelIconChange, XdgEvent,
};

/// Something luminaria reports, from whichever wrapper forwarded it.
#[derive(Debug)]
#[non_exhaustive]
pub enum Event {
    /// An output was lit up or went away.
    Backend(BackendEvent),
    /// Something happened on one output: it wants a frame, a frame reached the
    /// screen, or its mode changed.
    Output(OutputId, OutputEvent),
    /// Input from the nested window or libinput.
    Input(InputEvent),
    /// The parent compositor asked the nested window to close. Advisory: ending
    /// the session over it is the compositor's decision.
    HostCloseRequest,
    /// The VT came back (`true`) or was taken away (`false`). On the way out,
    /// `Seat::release_keys()`; on the way back, repaint.
    SessionActive(bool),
    Compositor(CompositorEvent),
    Xdg(XdgEvent),
    Seat(SeatEvent),
    Layer(SurfaceId, LayerSurfaceEvent),
    Decoration(DecorationRequest),
    CursorShape(CursorShapeRequest),
    ToplevelIcon(ToplevelIconChange),
    Activation(ActivationEvent),
    IdleInhibit(IdleInhibitEvent),
    IdleNotifier(IdleNotifierEvent),
    TextInput(TextInputEvent),
    PointerConstraint(PointerConstraintEvent),
    /// The clipboard or the primary selection changed owner.
    Selection(SelectionChange),
    #[cfg(feature = "desktop")]
    ForeignToplevel(ForeignToplevelRequest),
    #[cfg(feature = "desktop")]
    Workspace(WorkspaceRequest),
    #[cfg(feature = "desktop")]
    SessionLock(SessionLockEvent),
    #[cfg(feature = "desktop")]
    OutputConfig(OutputConfigRequest),
    #[cfg(feature = "desktop")]
    InputMethod(InputMethodEvent),
}

impl Event {
    fn from_raw(raw: ffi::EventRaw) -> Option<Self> {
        Some(match raw.kind {
            K::OutputRemoved => Event::Backend(BackendEvent::OutputRemoved(OutputId(raw.id))),
            K::OutputFrame => Event::Output(OutputId(raw.id), OutputEvent::Frame),
            K::OutputModeChanged => Event::Output(OutputId(raw.id), OutputEvent::ModeChanged),
            K::OutputPresent => Event::Output(
                OutputId(raw.id),
                OutputEvent::Present(PresentEvent {
                    tv_sec: raw.other,
                    tv_nsec: raw.serial,
                    refresh_ns: raw.value,
                    // Split across x and y by make_output_shim.
                    seq: u64::from(raw.x as u32) | u64::from(raw.y as u32) << 32,
                    vsync: raw.flag,
                    hw_clock: raw.flag2,
                }),
            ),
            K::HostCloseRequest => Event::HostCloseRequest,
            K::SessionActive => Event::SessionActive(true),
            K::SessionInactive => Event::SessionActive(false),
            K::NewSurface | K::SurfaceCommit | K::SurfaceDestroy | K::SurfaceRendered | K::SurfaceInvalidated => {
                Event::Compositor(CompositorEvent::from_raw(raw)?)
            }
            K::SeatKeyboardFocus | K::SeatPointerFocus | K::SeatCursorChange => Event::Seat(SeatEvent::from_raw(raw)?),
            K::NewLayerSurface
            | K::LayerSurfaceMap
            | K::LayerSurfaceUnmap
            | K::LayerSurfaceDestroy
            | K::LayerSurfaceStateChange
            | K::LayerSurfacePopup => {
                let (surface, event) = LayerSurfaceEvent::from_raw(raw)?;
                Event::Layer(surface, event)
            }
            K::DecorationRequest => Event::Decoration(DecorationRequest::from_raw(raw)?),
            K::CursorShapeRequest => Event::CursorShape(CursorShapeRequest::from_raw(raw)?),
            K::ToplevelIconChange => Event::ToplevelIcon(ToplevelIconChange::from_raw(raw)?),
            K::ActivationRequest | K::ActivationTokenRequest => Event::Activation(ActivationEvent::from_raw(raw)?),
            K::NewIdleInhibitor | K::IdleInhibitorDestroy | K::IdleInhibitChange => {
                Event::IdleInhibit(IdleInhibitEvent::from_raw(raw)?)
            }
            K::NewIdleNotification | K::IdleNotificationDestroy | K::IdleStateChange => {
                Event::IdleNotifier(IdleNotifierEvent::from_raw(raw)?)
            }
            K::NewTextInput | K::TextInputEnable | K::TextInputDisable | K::TextInputCommit | K::TextInputDestroy => {
                Event::TextInput(TextInputEvent::from_raw(raw)?)
            }
            K::NewPointerConstraint | K::PointerConstraintDestroy | K::PointerConstraintRegionChange => {
                Event::PointerConstraint(PointerConstraintEvent::from_raw(raw)?)
            }
            K::SelectionChange | K::PrimarySelectionChange => Event::Selection(SelectionChange::from_raw(raw)?),
            #[cfg(feature = "desktop")]
            K::ForeignToplevelRequest => Event::ForeignToplevel(ForeignToplevelRequest::from_raw(raw)?),
            #[cfg(feature = "desktop")]
            K::WorkspaceRequest => Event::Workspace(WorkspaceRequest::from_raw(raw)?),
            #[cfg(feature = "desktop")]
            K::NewSessionLock | K::NewLockSurface | K::LockSurfaceMap | K::LockSurfaceDestroy | K::SessionLockDestroy => {
                Event::SessionLock(SessionLockEvent::from_raw(raw)?)
            }
            #[cfg(feature = "desktop")]
            K::OutputConfigRequest => Event::OutputConfig(OutputConfigRequest::from_raw(raw)?),
            #[cfg(feature = "desktop")]
            K::NewInputMethod
            | K::InputMethodGrabChange
            | K::InputMethodDestroy
            | K::NewInputPopupSurface
            | K::InputPopupSurfaceDestroy => Event::InputMethod(InputMethodEvent::from_raw(raw)?),
            _ => Event::Xdg(XdgEvent::from_raw(&raw)?),
        })
    }
}

/// What one [`Display::dispatch`] collected, oldest first.
pub struct Events {
    inner: std::vec::IntoIter<Event>,
}

impl Iterator for Events {
    type Item = Event;

    fn next(&mut self) -> Option<Event> {
        self.inner.next()
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        self.inner.size_hint()
    }
}

impl ExactSizeIterator for Events {}

impl fmt::Debug for Events {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_list().entries(self.inner.as_slice()).finish()
    }
}

impl Display {
    /// Everything queued since the last collection, without waiting or
    /// dispatching: for a compositor that drives the event loop itself (calloop
    /// on [`Display`]'s fd, say). [`Display::dispatch`] ends with this.
    pub fn events(&self) -> Events {
        let mut out = Vec::new();
        loop {
            let mut raw = crate::ffi::EventRaw::empty();
            let mut input = InputEvent::empty_raw();
            match self.raw.next_event(&mut raw, &mut input) {
                0 => break,
                2 => out.push(Event::Input(InputEvent::from_raw(input))),
                3 => {
                    let output = self.raw.take_output();
                    if !output.is_null() {
                        out.push(Event::Backend(BackendEvent::OutputAdded(Output::from_raw(output))));
                    }
                }
                _ => out.extend(Event::from_raw(raw)),
            }
        }
        Events { inner: out.into_iter() }
    }
}

/// `TryFrom<Event>` for each payload, giving back the event when it is
/// something else.
macro_rules! payload {
    ($($variant:ident($ty:ty)),* $(,)?) => {$(
        impl TryFrom<Event> for $ty {
            type Error = Event;
            fn try_from(event: Event) -> Result<Self, Event> {
                match event {
                    Event::$variant(payload) => Ok(payload),
                    other => Err(other),
                }
            }
        }
        impl From<$ty> for Event {
            fn from(payload: $ty) -> Self {
                Event::$variant(payload)
            }
        }
    )*};
}

payload! {
    Backend(BackendEvent),
    Input(InputEvent),
    Compositor(CompositorEvent),
    Xdg(XdgEvent),
    Seat(SeatEvent),
    Decoration(DecorationRequest),
    CursorShape(CursorShapeRequest),
    ToplevelIcon(ToplevelIconChange),
    Activation(ActivationEvent),
    IdleInhibit(IdleInhibitEvent),
    IdleNotifier(IdleNotifierEvent),
    TextInput(TextInputEvent),
    PointerConstraint(PointerConstraintEvent),
    Selection(SelectionChange),
}

#[cfg(feature = "desktop")]
payload! {
    ForeignToplevel(ForeignToplevelRequest),
    Workspace(WorkspaceRequest),
    SessionLock(SessionLockEvent),
    OutputConfig(OutputConfigRequest),
    InputMethod(InputMethodEvent),
}

impl TryFrom<Event> for (OutputId, OutputEvent) {
    type Error = Event;
    fn try_from(event: Event) -> Result<Self, Event> {
        match event {
            Event::Output(id, e) => Ok((id, e)),
            other => Err(other),
        }
    }
}

impl TryFrom<Event> for (SurfaceId, LayerSurfaceEvent) {
    type Error = Event;
    fn try_from(event: Event) -> Result<Self, Event> {
        match event {
            Event::Layer(surface, e) => Ok((surface, e)),
            other => Err(other),
        }
    }
}
