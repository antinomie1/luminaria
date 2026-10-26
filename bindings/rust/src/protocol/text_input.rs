//! text-input-v3: a client's text field talking to an input method.

use crate::core::{Display, Error, Holds};
use crate::ffi::{self, EventKind};
use crate::util::Rect;

use super::{Seat, SurfaceId};

/// A text input, named by the binding: a client has one per seat, not per field.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct TextInputId(pub u64);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TextInputEvent {
    New(TextInputId),
    /// A text field took focus and wants input.
    Enable(TextInputId),
    Disable(TextInputId),
    /// The client committed new state (surrounding text, content type,
    /// cursor rectangle).
    Commit(TextInputId),
    Destroy(TextInputId),
}

/// The `zwp_text_input_manager_v3` global. It routes through the [`Seat`] it
/// was created for, which it keeps borrowed; an
/// [`InputMethodManager`](crate::desktop::InputMethodManager) keeps it borrowed
/// in turn and bridges the two by itself.
pub struct TextInputManager<'a> {
    pub(crate) raw: cxx::UniquePtr<ffi::TextInputManagerShim>,
    _borrows: Holds<'a>,
}

impl<'a> TextInputManager<'a> {
    pub fn new(display: &'a Display, seat: &'a Seat<'_>) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::text_input_manager_new(&display.raw, &seat.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _borrows: Holds::new() })
        }
    }
}

impl TextInputManager<'_> {
    pub fn next_event(&self) -> Option<TextInputEvent> {
        loop {
            let raw = crate::next_raw_event(|out| self.raw.next_event(out))?;
            let id = TextInputId(raw.id);
            return Some(match raw.kind {
                EventKind::NewTextInput => TextInputEvent::New(id),
                EventKind::TextInputEnable => TextInputEvent::Enable(id),
                EventKind::TextInputDisable => TextInputEvent::Disable(id),
                EventKind::TextInputCommit => TextInputEvent::Commit(id),
                EventKind::TextInputDestroy => TextInputEvent::Destroy(id),
                _ => continue,
            });
        }
    }

    /// The enabled text input of the keyboard-focused client.
    pub fn focused(&self) -> Option<TextInputId> {
        match self.raw.focused() {
            0 => None,
            id => Some(TextInputId(id)),
        }
    }

    pub fn text_input(&self, id: TextInputId) -> Option<TextInput<'_>> {
        let mut info = ffi::TextInputInfoRaw::default();
        self.raw
            .text_input_info(id.0, &mut info)
            .then_some(TextInput { mgr: self.raw.as_ref()?, id })
    }
}

/// A text input's committed state, and the events that answer it. Looked up
/// on every call.
#[derive(Clone, Copy)]
pub struct TextInput<'m> {
    mgr: &'m ffi::TextInputManagerShim,
    id: TextInputId,
}

/// The parts of a text input's state that are plain values.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct TextInputState {
    /// The surface it was told about with `enter`.
    pub focused_surface: Option<SurfaceId>,
    pub enabled: bool,
    /// Byte offsets into [`TextInput::surrounding_text`].
    pub cursor: u32,
    pub anchor: u32,
    /// zwp_text_input_v3.change_cause: 0 input method, 1 other.
    pub change_cause: u32,
    /// zwp_text_input_v3.content_hint bits.
    pub content_hint: u32,
    /// zwp_text_input_v3.content_purpose.
    pub content_purpose: u32,
    /// The cursor, in surface coordinates: where a candidate window goes.
    pub cursor_rectangle: Rect,
    /// How many commits the client has made; answers carry it.
    pub serial: u32,
}

impl TextInput<'_> {
    pub fn id(&self) -> TextInputId {
        self.id
    }

    pub fn state(&self) -> TextInputState {
        let mut i = ffi::TextInputInfoRaw::default();
        self.mgr.text_input_info(self.id.0, &mut i);
        TextInputState {
            focused_surface: SurfaceId::from_raw(i.focused_surface),
            enabled: i.enabled,
            cursor: i.cursor,
            anchor: i.anchor,
            change_cause: i.change_cause,
            content_hint: i.content_hint,
            content_purpose: i.content_purpose,
            cursor_rectangle: i.cursor_rectangle,
            serial: i.serial,
        }
    }

    pub fn surrounding_text(&self) -> String {
        self.mgr.surrounding_text(self.id.0)
    }

    // The send_* calls queue state the client applies at the next send_done.
    pub fn send_preedit_string(&self, text: &str, cursor_begin: i32, cursor_end: i32) {
        self.mgr.send_preedit_string(self.id.0, text, cursor_begin, cursor_end);
    }

    pub fn send_commit_string(&self, text: &str) {
        self.mgr.send_commit_string(self.id.0, text);
    }

    pub fn send_delete_surrounding_text(&self, before: u32, after: u32) {
        self.mgr.send_delete_surrounding_text(self.id.0, before, after);
    }

    pub fn send_done(&self) {
        self.mgr.send_done(self.id.0);
    }
}
