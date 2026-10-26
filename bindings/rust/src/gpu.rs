//! GPU rendering, Vulkan integration, direct scanout, and hardware frame ledgers.

use std::os::fd::AsFd;
use std::os::fd::AsRawFd;
use std::pin::Pin;

use crate::backend::Output;
use crate::core::{Display, Error};
use crate::ffi;
use crate::protocol::{Presentation, Seat, SurfaceId};
use crate::shell::{OutputLayout, SceneItem};
use crate::util::{Color, Rect};

pub use crate::ffi::DrmDeviceId;

impl DrmDeviceId {
    /// The device an open card or render node refers to; `None` if it is not
    /// a character device. `DrmDeviceId::from_fd(&drm_backend)` is how the
    /// renderer is made to be the GPU the display belongs to.
    pub fn from_fd(fd: impl AsFd) -> Option<Self> {
        let mut id = DrmDeviceId { major: 0, minor: 0 };
        ffi::drm_device_id_from_fd(fd.as_fd().as_raw_fd(), &mut id).then_some(id)
    }

    /// The device a path such as `/dev/dri/card0` names.
    pub fn from_path(path: &str) -> Option<Self> {
        let mut id = DrmDeviceId { major: 0, minor: 0 };
        ffi::drm_device_id_from_path(path, &mut id).then_some(id)
    }
}

/// Options for initializing Vulkan and physical device selection.
#[derive(Debug, Clone, Default)]
pub struct VulkanOptions {
    /// The DRM device the compositor scans out through. The physical device
    /// behind it wins over every other; `None` lets the ranking decide.
    pub drm_device: Option<DrmDeviceId>,
}

impl VulkanOptions {
    fn drm_device_raw(&self) -> (bool, DrmDeviceId) {
        match self.drm_device {
            Some(id) => (true, id),
            None => (false, DrmDeviceId { major: 0, minor: 0 }),
        }
    }
}

/// The renderer's Vulkan objects as raw handles (`raw-vulkan` feature).
///
/// Every handle is a `u64`, which is what ash's `Handle::from_raw` takes:
/// `ash::Instance::load(entry.static_fn(), vk::Instance::from_raw(h.instance))`.
///
/// Borrowed: valid until the renderer drops, never to be destroyed. The queue
/// needs external synchronisation, so submit to it only from the thread that
/// drives luminaria, between its calls, and wait for your own work before the
/// renderer goes; luminaria's timeline does not know about it. The device has
/// exactly `device_extensions` plus Vulkan 1.3's synchronization2,
/// dynamicRendering and timelineSemaphore. See `docs/api.md` §9.4.
#[cfg(feature = "raw-vulkan")]
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawVulkanHandles {
    pub instance: u64,
    pub physical_device: u64,
    pub device: u64,
    pub queue: u64,
    pub queue_family_index: u32,
    /// What the instance was created for (`VK_API_VERSION_1_3`).
    pub api_version: u32,
    pub device_extensions: Vec<String>,
}

#[cfg(feature = "raw-vulkan")]
impl From<ffi::VulkanHandlesRaw> for RawVulkanHandles {
    fn from(h: ffi::VulkanHandlesRaw) -> Self {
        Self {
            instance: h.instance,
            physical_device: h.physical_device,
            device: h.device,
            queue: h.queue,
            queue_family_index: h.queue_family,
            api_version: h.api_version,
            device_extensions: h.device_extensions,
        }
    }
}

/// Vulkan 1.3 renderer with external memory dmabuf and dynamic rendering.
pub struct VulkanRenderer {
    pub(crate) raw: cxx::UniquePtr<ffi::VulkanRendererShim>,
}

impl VulkanRenderer {
    pub fn new(options: &VulkanOptions) -> Result<Self, Error> {
        let mut err = String::new();
        let (has_drm, drm) = options.drm_device_raw();
        let raw = ffi::vulkan_renderer_new(has_drm, &drm, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn scanout_modifiers(&self, drm_format: u32) -> Vec<u64> {
        self.raw.scanout_modifiers(drm_format)
    }

    /// The instance, device and queue this renderer runs on, for ash.
    #[cfg(feature = "raw-vulkan")]
    pub fn raw_handles(&self) -> RawVulkanHandles {
        self.raw.handles().into()
    }
}

/// The `zwp_linux_dmabuf_v1` global.
pub struct LinuxDmabuf {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::LinuxDmabufShim>,
}

impl LinuxDmabuf {
    pub fn new(display: &Display, renderer: Option<&VulkanRenderer>) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = if let Some(vk) = renderer {
            ffi::linux_dmabuf_new(&display.raw, &vk.raw, &mut err)
        } else {
            ffi::linux_dmabuf_new_headless(&display.raw, &mut err)
        };
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

/// The `wp_linux_drm_syncobj_manager_v1` global for explicit synchronization.
pub struct DrmSyncobjManager {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::DrmSyncobjManagerShim>,
}

impl DrmSyncobjManager {
    pub fn new(display: &Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::drm_syncobj_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

/// The screencopy protocol manager (`zwlr_screencopy_manager_v1` and `ext_image_copy_capture_manager_v1`).
pub struct ScreencopyManager {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::ScreencopyManagerShim>,
}

impl ScreencopyManager {
    pub fn new(display: &Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::screencopy_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }
}

/// Per-output scene rendering state (GPU ledger or CPU scratch).
pub struct SceneOutput {
    pub(crate) raw: cxx::UniquePtr<ffi::SceneOutputShim>,
}

impl SceneOutput {
    pub fn new() -> Self {
        Self {
            raw: ffi::scene_output_new(),
        }
    }

    pub fn gpu_output(&self) -> bool {
        self.raw.gpu_output()
    }
}

impl Default for SceneOutput {
    fn default() -> Self {
        Self::new()
    }
}

/// Outcome of presenting a scene.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SceneOutcome {
    Composited = 0,
    Scanout = 1,
    Unchanged = 2,
    Cpu = 3,
}

impl SceneOutcome {
    fn from_raw(code: i32) -> Self {
        match code {
            0 => Self::Composited,
            1 => Self::Scanout,
            2 => Self::Unchanged,
            _ => Self::Cpu,
        }
    }
}

/// Unified scene presenter over Vulkan GPU and CPU fallback.
pub struct SceneRenderer {
    pub(crate) raw: cxx::UniquePtr<ffi::SceneRendererShim>,
}

impl SceneRenderer {
    pub fn new(options: &VulkanOptions) -> Result<Self, Error> {
        let mut err = String::new();
        let (has_drm, drm) = options.drm_device_raw();
        let raw = ffi::scene_renderer_new(has_drm, &drm, &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    fn pin(&mut self) -> Pin<&mut ffi::SceneRendererShim> {
        self.raw.pin_mut()
    }

    pub fn gpu_enabled(&self) -> bool {
        self.raw.gpu_enabled()
    }

    pub fn gpu_error(&self) -> String {
        self.raw.gpu_error()
    }

    pub fn reset(
        &mut self,
        state: &mut SceneOutput,
        output: &mut Output,
        drm_format: u32,
    ) -> Result<(), Error> {
        let mut err = String::new();
        let mut s_pin = state.raw.pin_mut();
        let mut o_pin = output.raw.pin_mut();
        if self
            .pin()
            .reset(s_pin.as_mut(), o_pin.as_mut(), drm_format, &mut err)
        {
            Ok(())
        } else {
            Err(Error(err))
        }
    }

    pub fn present(
        &mut self,
        state: &mut SceneOutput,
        output: &mut Output,
        items: &[SceneItem],
        background: Color,
    ) -> Result<SceneOutcome, Error> {
        let raw_items: Vec<ffi::SceneItemRaw> = items.iter().map(SceneItem::to_raw).collect();
        let mut err = String::new();
        let mut s_pin = state.raw.pin_mut();
        let mut o_pin = output.raw.pin_mut();
        let res = self.pin().present(
            s_pin.as_mut(),
            o_pin.as_mut(),
            &raw_items,
            background.r,
            background.g,
            background.b,
            background.a,
            &mut err,
        );
        if res < 0 {
            Err(Error(err))
        } else {
            Ok(SceneOutcome::from_raw(res))
        }
    }

    pub fn set_presentation(&mut self, presentation: Option<&Presentation>) {
        if let Some(pres) = presentation {
            self.pin().set_presentation(&pres.raw);
        } else {
            self.pin().clear_presentation();
        }
    }

    pub fn update_cursor(&mut self) {
        self.pin().update_cursor();
    }

    /// The GPU's raw Vulkan handles; `None` when this renderer fell back to the
    /// CPU. Same contract as [`VulkanRenderer::raw_handles`].
    #[cfg(feature = "raw-vulkan")]
    pub fn raw_vulkan_handles(&self) -> Option<RawVulkanHandles> {
        let mut h = ffi::VulkanHandlesRaw::default();
        self.raw.vulkan_handles(&mut h).then(|| h.into())
    }

    pub fn read_back(&mut self, state: &mut SceneOutput) -> Result<Vec<u8>, Error> {
        let mut err = String::new();
        let mut s_pin = state.raw.pin_mut();
        let vec = self.pin().read_back(s_pin.as_mut(), &mut err);
        if vec.is_empty() && !err.is_empty() {
            Err(Error(err))
        } else {
            Ok(vec)
        }
    }
}

/// The cursor/pointer presentation manager: KMS planes, sprite themes, and screencopy.
pub struct Cursor {
    raw: cxx::UniquePtr<ffi::CursorShim>,
}

impl Cursor {
    pub fn new(
        renderer: &mut SceneRenderer,
        layout: &OutputLayout,
        seat: &mut Seat,
    ) -> Result<Self, Error> {
        let mut err = String::new();
        let mut r_pin = renderer.raw.pin_mut();
        let mut s_pin = seat.raw.pin_mut();
        let raw = ffi::cursor_new(r_pin.as_mut(), &layout.raw, s_pin.as_mut(), &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    fn pin(&mut self) -> Pin<&mut ffi::CursorShim> {
        self.raw.pin_mut()
    }

    pub fn move_to(&mut self, x: f64, y: f64, bounds: Rect) -> bool {
        self.pin().move_to(x, y, bounds)
    }

    pub fn move_by(&mut self, dx: f64, dy: f64, bounds: Rect) -> bool {
        self.pin().move_by(dx, dy, bounds)
    }

    pub fn leave(&mut self) {
        self.pin().leave();
    }

    /// Where the pointer is, in layout coordinates, after the clamping
    /// `move_to`/`move_by` did; `None` before the first move or after `leave()`.
    pub fn position(&self) -> Option<(f64, f64)> {
        let (mut x, mut y) = (0.0, 0.0);
        self.raw.position(&mut x, &mut y).then_some((x, y))
    }

    pub fn refresh(&mut self) {
        self.pin().refresh();
    }
}

/// Outcome of presenting a Frame ledger.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Presented {
    Composited = 0,
    Scanout = 1,
    Unchanged = 2,
    Fallback = 3,
}

impl Presented {
    fn from_raw(code: i32) -> Self {
        match code {
            0 => Self::Composited,
            1 => Self::Scanout,
            2 => Self::Unchanged,
            _ => Self::Fallback,
        }
    }
}

/// The per-output immediate-mode GPU frame ledger.
pub struct Frame {
    raw: cxx::UniquePtr<ffi::FrameShim>,
}

impl Frame {
    pub fn new(output: &mut Output, renderer: &mut VulkanRenderer) -> Result<Self, Error> {
        let mut err = String::new();
        let mut o_pin = output.raw.pin_mut();
        let mut r_pin = renderer.raw.pin_mut();
        let raw = ffi::frame_new(o_pin.as_mut(), r_pin.as_mut(), &mut err);
        if raw.is_null() {
            Err(Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    fn pin(&mut self) -> Pin<&mut ffi::FrameShim> {
        self.raw.pin_mut()
    }

    pub fn reset(&mut self, drm_format: u32) -> Result<(), Error> {
        let mut err = String::new();
        if self.pin().reset(drm_format, &mut err) {
            Ok(())
        } else {
            Err(Error(err))
        }
    }

    pub fn begin(&mut self, viewport: Rect) {
        self.pin()
            .begin(viewport.x, viewport.y, viewport.width, viewport.height);
    }

    pub fn place_rect(&mut self, rect: Rect, color: Color) {
        self.pin().place_rect(
            rect.x,
            rect.y,
            rect.width,
            rect.height,
            color.r,
            color.g,
            color.b,
            color.a,
        );
    }

    pub fn submit(&mut self, background: Color) -> Result<Presented, Error> {
        let mut err = String::new();
        let res = self.pin().submit(
            background.r,
            background.g,
            background.b,
            background.a,
            &mut err,
        );
        if res < 0 {
            Err(Error(err))
        } else {
            Ok(Presented::from_raw(res))
        }
    }

    pub fn send_frame_done(&mut self, time_ms: u32) {
        self.pin().send_frame_done(time_ms);
    }
}

/// Direct client scanout bypass evaluator.
pub struct DirectScanout {
    raw: cxx::UniquePtr<ffi::DirectScanoutShim>,
}

impl DirectScanout {
    pub fn new(output: &mut Output) -> Result<Self, Error> {
        let mut o_pin = output.raw.pin_mut();
        let raw = ffi::direct_scanout_new(o_pin.as_mut());
        if raw.is_null() {
            Err(Error("failed to initialize direct scanout".into()))
        } else {
            Ok(Self { raw })
        }
    }

    fn pin(&mut self) -> Pin<&mut ffi::DirectScanoutShim> {
        self.raw.pin_mut()
    }

    pub fn available(&self) -> bool {
        self.raw.available()
    }

    pub fn id_for(&mut self, surface: SurfaceId) -> Option<u32> {
        match self.pin().id_for(surface.0) {
            0 => None,
            id => Some(id),
        }
    }

    pub fn committed(&mut self, surface: SurfaceId) {
        self.pin().committed(surface.0);
    }

    pub fn presented(&mut self) {
        self.pin().presented();
    }

    pub fn clear(&mut self) {
        self.pin().clear();
    }

    pub fn cached(&self) -> usize {
        self.raw.cached()
    }
}
