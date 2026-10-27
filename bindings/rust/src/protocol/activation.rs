//! xdg-activation: a client asking for one of its windows to be focused.

use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi::{self, EventKind};

use super::SurfaceId;

/// An event from [`XdgActivation`]: a token request or an activation request.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ActivationEvent {
    Request(ActivationRequest),
    TokenRequest(ActivationTokenRequest),
}

/// A client asked for an activation token (`xdg_activation_token_v1.commit`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ActivationTokenRequest {
    pub surface: Option<SurfaceId>,
    pub serial: u32,
    pub app_id: String,
    pub granted: bool,
}

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

/// The `xdg_activation_v1` global.
pub struct XdgActivation<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::XdgActivationShim>,
    _display: Holds<'d>,
}

impl<'d> XdgActivation<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::xdg_activation_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self { raw, _display: Holds::new() })
        }
    }

    /// Whether to grant future token requests. Default is true.
    pub fn set_grant_tokens(&self, grant: bool) {
        self.raw.set_grant_tokens(grant);
    }
}

impl ActivationEvent {
    #[allow(unreachable_code, clippy::needless_return)]
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<ActivationEvent> {
        match raw.kind {
            EventKind::ActivationRequest => {
                return Some(ActivationEvent::Request(ActivationRequest {
                    surface: SurfaceId(raw.id),
                    token_valid: raw.flag,
                    app_id: raw.text,
                    requesting_surface: SurfaceId::from_raw(raw.other),
                    serial: raw.serial,
                }));
            }
            EventKind::ActivationTokenRequest => {
                return Some(ActivationEvent::TokenRequest(ActivationTokenRequest {
                    surface: SurfaceId::from_raw(raw.id),
                    serial: raw.serial,
                    app_id: raw.text,
                    granted: raw.flag,
                }));
            }
            _ => {}
        }
        None
    }
}
