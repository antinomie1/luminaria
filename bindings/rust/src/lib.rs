//! Rust bindings for luminaria, a Wayland compositor library.
//!
//! Luminaria provides three C++23 modules, and this crate mirrors them type for type,
//! with one `#[cxx::bridge]` per module (ADR 0003):
//! - `luminaria`: Core protocols, utilities, event loop, display, and nested/headless backends.
//! - `luminaria.gpu`: Vulkan renderer, DRM/KMS, libinput, session, screencopy, direct scanout, and GPU frame ledgers.
//! - `luminaria.desktop`: Privileged desktop-shell protocols (foreign-toplevel, workspaces, session-lock, output-management, data-control, input-method).
//! - `luminaria.text`: Glyph rasterization and font metrics (fcft + pixman).
//!
//! The two extensions are cargo features, both on by default: `default = ["gpu", "desktop"]`.
//! Turning one off stops the crate from compiling and linking the C++ module behind
//! it, so a headless-only user does not pay for Vulkan, libdrm, libinput, libudev,
//! gbm or libseat. `--no-default-features` is the core library and nothing else.
//!
//! | feature | C++ module | what it adds | system libraries |
//! |---|---|---|---|
//! | *(always)* | `luminaria` | protocols, Display, headless + nested backends | wayland-server, wayland-client, xkbcommon |
//! | `gpu` | `luminaria.gpu` | [`gpu`], DRM/libinput/session backends | vulkan, libdrm, libinput, libudev, gbm, libseat |
//! | `desktop` | `luminaria.desktop` | [`desktop`], implied by nothing | — (implies `gpu`) |
//! | `text` | `luminaria.text` | `text` | fcft, fontconfig, pixman |
//!
//! A compositor is assembled from the submodules; the policy (where windows go,
//! who has the keyboard, what a key does) stays in the caller:
//! - [`util`]: Rect, Color, Transform, Region, KeymapState, Pixel, resize edges.
//! - [`core`]: Display, EventLoop, Error, KeyRouter.
//! - [`backend`]: Output, HeadlessBackend, WaylandBackend, and (with `gpu`) DrmBackend, LibinputBackend, Session.
//! - [`protocol`]: Compositor, Subcompositor, XdgShell, Seat, LayerShell, and core protocol managers.
//! - [`gpu`]: VulkanRenderer, SceneRenderer, Frame, Cursor, DirectScanout, LinuxDmabuf, Screencopy.
//! - [`desktop`]: ForeignToplevelManager, WorkspaceManager, SessionLockManager, OutputManager, DataControlManager, InputMethodManager.
//! - [`shell`]: OutputLayout, SceneItem, SceneHit, scene_hit_test, CpuCompositor, PopupTree, LayerManager.
//!
//! The library is driven by its signals, and C++ never calls Rust: each global
//! queues what it emitted, and its `next_event()` hands it over after
//! [`Display::dispatch`](core::Display::dispatch) returns. The [`protocol`]
//! module documentation says how the objects those events are about are named.
//!
//! `examples/minimal.rs` is a complete compositor built this way.

pub mod backend;
pub mod core;
#[cfg(feature = "desktop")]
pub mod desktop;
#[cfg(feature = "gpu")]
pub mod gpu;
pub mod protocol;
pub mod shell;
#[cfg(feature = "text")]
pub mod text;
pub mod util;

// One bridge per C++ module. Each file holds a `#[cxx::bridge] mod ffi`; the
// crate root re-exports them as `crate::ffi`, `crate::ffi_gpu`, ... so the
// wrappers can name `crate::ffi::Rect` and `crate::ffi_gpu::VulkanRendererShim`.
#[path = "ffi.rs"]
mod core_bridge;
pub(crate) use core_bridge::ffi;

#[cfg(feature = "gpu")]
#[path = "ffi_gpu.rs"]
mod gpu_bridge;
#[cfg(feature = "gpu")]
pub(crate) use gpu_bridge::ffi as ffi_gpu;

#[cfg(feature = "desktop")]
#[path = "ffi_desktop.rs"]
mod desktop_bridge;
#[cfg(feature = "desktop")]
pub(crate) use desktop_bridge::ffi as ffi_desktop;

#[cfg(feature = "text")]
#[path = "ffi_text.rs"]
mod text_bridge;
#[cfg(feature = "text")]
pub(crate) use text_bridge::ffi as ffi_text;

impl ffi::EventRaw {
    fn empty() -> Self {
        Self {
            kind: ffi::EventKind::None,
            id: 0,
            other: 0,
            serial: 0,
            x: 0,
            y: 0,
            value: 0,
            flag: false,
            flag2: false,
            text: String::new(),
        }
    }
}

/// Pops the shim's next queued event, if any.
pub(crate) fn next_raw_event(next: impl FnOnce(&mut ffi::EventRaw) -> bool) -> Option<ffi::EventRaw> {
    let mut raw = ffi::EventRaw::empty();
    next(&mut raw).then_some(raw)
}

