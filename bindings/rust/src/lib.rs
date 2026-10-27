//! Rust bindings for luminaria, a Wayland compositor library.
//!
//! Luminaria provides four C++23 modules, and this crate mirrors them type for type,
//! with one `#[cxx::bridge]` per module (ADR 0008):
//! - `luminaria`: the mechanism kernel, the value types, the `Backend`/`Output`
//!   abstraction, and every protocol whose semantics are plain Wayland.
//! - `luminaria.backend`: the outputs and the input devices — headless, nested,
//!   DRM/KMS, libinput, libseat.
//! - `luminaria.render`: Vulkan, the CPU compositor, and the buffer protocols
//!   (linux-dmabuf, drm-syncobj, screencopy).
//! - `luminaria.shell`: the immediate-mode pipeline — `Frame`, `SceneRenderer`,
//!   `Cursor`, layer surfaces, popups, direct scanout, `AutoBackend`.
//! - `luminaria.desktop`: privileged desktop-shell protocols (foreign-toplevel,
//!   workspaces, session-lock, output-management, data-control, input-method).
//! - `luminaria.text`: glyph rasterization and font metrics (fcft + pixman).
//!
//! The extensions are cargo features: `gpu` (the historical name for the whole
//! stack, still the default), `shell` (which implies `backend` + `render`),
//! `backend`, `render`, `desktop` and `text`. Turning one off stops the crate from
//! compiling and linking the C++ module behind it, so a headless-only user pays
//! for none of Vulkan, libdrm, libinput, libudev, gbm or libseat.
//! `--no-default-features` is the core library and nothing else.
//!
//! | feature | C++ module | what it adds | system libraries |
//! |---|---|---|---|
//! | *(always)* | `luminaria` | protocols, Display, value types | wayland-server, wayland-client, xkbcommon |
//! | `backend` | `luminaria.backend` | headless, nested, DRM, libinput, session | libdrm, libinput, libudev, libseat |
//! | `render` | `luminaria.render` | Vulkan, the CPU compositor, dmabuf/syncobj/screencopy | vulkan, libdrm, gbm |
//! | `shell`/`gpu` | `luminaria.shell` | [`gpu::Frame`], [`gpu::Cursor`], [`gpu::SceneRenderer`], [`backend`] backends | the two rows above |
//! | `desktop` | `luminaria.desktop` | [`desktop`] | — |
//! | `text` | `luminaria.text` | `text` | fcft, fontconfig, pixman |
//!
//! A compositor is assembled from the submodules; the policy (where windows go,
//! who has the keyboard, what a key does) stays in the caller:
//! - [`util`]: Rect, Color, Transform, Region, KeymapState, Pixel, resize edges.
//! - [`core`]: Display, EventLoop, Error, KeyRouter.
//! - [`backend`]: Output, and (with `shell`) HeadlessBackend, WaylandBackend, DrmBackend, LibinputBackend, Session, and AutoBackend, which picks among them.
//! - [`protocol`]: Compositor, Subcompositor, XdgShell, Seat, LayerShell, and core protocol managers.
//! - [`gpu`]: VulkanRenderer, SceneRenderer, Frame, Cursor, DirectScanout, LinuxDmabuf, Screencopy.
//! - [`desktop`]: ForeignToplevelManager, WorkspaceManager, SessionLockManager, OutputManager, DataControlManager, InputMethodManager.
//! - [`event`]: Event, the one stream everything above reports through.
//! - [`shell`]: OutputLayout, SceneItem, SceneHit, and (with `shell`) scene_hit_test, CpuCompositor, PopupTree, LayerManager.
//!
//! The library is driven by its signals, and C++ never calls Rust: every
//! signal is queued in the order it fired, and [`Display::dispatch`](core::Display::dispatch)
//! hands them all back as one stream of [`Event`]s. The [`event`] module says
//! how the objects those events are about are named.
//!
//! `examples/minimal.rs` is a complete compositor built this way.

pub mod backend;
pub mod core;
#[cfg(feature = "desktop")]
pub mod desktop;
pub mod event;
#[cfg(feature = "shell")]
pub mod gpu;
pub mod protocol;
#[cfg(feature = "render")]
pub mod render;
pub mod shell;
#[cfg(feature = "text")]
pub mod text;
pub mod util;

pub use event::{Event, Events};

// One bridge per C++ module (ADR 0008). Each file holds a `#[cxx::bridge] mod ffi`; the
// crate root re-exports them as `crate::ffi`, `crate::ffi_backend`, ... so the
// wrappers can name `crate::ffi::Rect` and `crate::ffi_render::VulkanRendererShim`.
#[path = "ffi.rs"]
mod core_bridge;
pub(crate) use core_bridge::ffi;

#[cfg(feature = "backend")]
#[path = "ffi_backend.rs"]
mod backend_bridge;
#[cfg(feature = "backend")]
pub(crate) use backend_bridge::ffi as ffi_backend;

#[cfg(feature = "render")]
#[path = "ffi_render.rs"]
mod render_bridge;
#[cfg(feature = "render")]
pub(crate) use render_bridge::ffi as ffi_render;

#[cfg(feature = "shell")]
#[path = "ffi_shell.rs"]
mod shell_bridge;
#[cfg(feature = "shell")]
pub(crate) use shell_bridge::ffi as ffi_shell;

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
    pub(crate) fn empty() -> Self {
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
