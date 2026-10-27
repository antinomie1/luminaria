//! The clipboard (wl_data_device) and the middle-click selection
//! (primary-selection).

use std::os::fd::{IntoRawFd, OwnedFd};

use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi;

use super::Seat;

macro_rules! selection_manager {
    ($(#[$doc:meta])* $name:ident, $shim:ident, $new:ident) => {
        $(#[$doc])*
        pub struct $name<'a> {
            pub(crate) raw: cxx::UniquePtr<ffi::$shim>,
            _borrows: Holds<'a>,
        }

        impl<'a> $name<'a> {
            pub fn new(display: &'a Display, seat: &'a Seat<'_>) -> Result<Self, Error> {
                let mut err = String::new();
                let raw = ffi::$new(&display.raw, &seat.raw, &mut err);
                if raw.is_null() {
                    Err(Error::new(ErrorKind::Protocol, err))
                } else {
                    Ok(Self { raw, _borrows: Holds::new() })
                }
            }
        }

        impl $name<'_> {
            /// What the current selection offers; empty when there is none.
            pub fn selection_mime_types(&self) -> Vec<String> {
                self.raw.selection_mime_types()
            }

            /// Ask the selection's owner to write it as `mime` into `fd`, the
            /// write end of a pipe; read the other end after dispatching. The
            /// fd is consumed either way. False when there is no selection.
            pub fn selection_receive(&self, mime: &str, fd: OwnedFd) -> bool {
                self.raw.selection_receive(mime, fd.into_raw_fd())
            }
        }
    };
}

/// The selection changed owner: [`crate::Event::Selection`]. `mime_types` is
/// what the new owner offers; empty when the selection was cleared.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SelectionChange {
    /// The middle-click selection rather than the clipboard.
    pub primary: bool,
    pub mime_types: Vec<String>,
}

impl SelectionChange {
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<Self> {
        let primary = match raw.kind {
            crate::ffi::EventKind::SelectionChange => false,
            crate::ffi::EventKind::PrimarySelectionChange => true,
            _ => return None,
        };
        let mime_types = if raw.text.is_empty() {
            Vec::new()
        } else {
            raw.text.lines().map(str::to_owned).collect()
        };
        Some(Self {
            primary,
            mime_types,
        })
    }
}

selection_manager!(
    /// The `wl_data_device_manager` (clipboard and drag-and-drop) global. It
    /// routes through the [`Seat`] it was created for, which it keeps borrowed.
    /// Changes of owner arrive as [`crate::Event::Selection`].
    DataDeviceManager,
    DataDeviceManagerShim,
    data_device_manager_new
);

selection_manager!(
    /// The `zwp_primary_selection_device_manager_v1` global. It routes through
    /// the [`Seat`] it was created for, which it keeps borrowed.
    PrimarySelectionManager,
    PrimarySelectionManagerShim,
    primary_selection_manager_new
);
