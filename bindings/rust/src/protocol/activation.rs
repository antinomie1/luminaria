//! xdg-activation: a client asking for one of its windows to be focused.

use crate::core::{Display, Error, Holds};
use crate::ffi::{self, EventKind};

use super::SurfaceId;

/// A client wants `surface` focused. Whether it gets it is the compositor's
/// call: a valid token came from a recent input event, an invalid one did not.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ActivationRequest {
    pub surface: SurfaceId,
    pub token_valid: bool,
    /// The app id the token was issued for, if the issuer gave one.
    pub app_id: String,
    /// The surface that requested the token.
    pub requesting_surface: Option<SurfaceId>,
    /// The input serial the token was requested with.
    pub serial: u32,
}

/// The `xdg_activation_v1` global. Every token request is granted; the
/// decision is made inside the request, which Rust cannot answer.
pub struct XdgActivation<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::XdgActivationShim>,
    _display: Holds<'d>,
}

impl<'d> XdgActivation<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::xdg_activation_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }
}

impl XdgActivation<'_> {
    pub fn next_event(&self) -> Option<ActivationRequest> {
        loop {
            let raw = crate::next_raw_event(|out| self.raw.next_event(out))?;
            if raw.kind != EventKind::ActivationRequest {
                continue;
            }
            return Some(ActivationRequest {
                surface: SurfaceId(raw.id),
                token_valid: raw.flag,
                app_id: raw.text,
                requesting_surface: SurfaceId::from_raw(raw.other),
                serial: raw.serial,
            });
        }
    }
}
