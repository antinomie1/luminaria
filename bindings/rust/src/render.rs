//! Vulkan rendering, CPU compositing, and buffer protocols.

use std::os::fd::AsFd;
use std::os::fd::AsRawFd;

use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi_render as ffi;
use crate::util::{Color, DrmFourcc, Rect};

pub use crate::ffi_render::DrmDeviceId;

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
    pub(crate) fn drm_device_raw(&self) -> (bool, DrmDeviceId) {
        match self.drm_device {
            Some(id) => (true, id),
            None => (false, DrmDeviceId { major: 0, minor: 0 }),
        }
    }
}

/// The renderer's Vulkan objects as raw handles (`raw-vulkan` feature).
#[cfg(feature = "raw-vulkan")]
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawVulkanHandles {
    pub instance: u64,
    pub physical_device: u64,
    pub device: u64,
    pub queue: u64,
    pub queue_family_index: u32,
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
            Err(Error::new(ErrorKind::Gpu, err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn scanout_modifiers(&self, format: DrmFourcc) -> Vec<u64> {
        self.raw.scanout_modifiers(format as u32)
    }

    /// The instance, device and queue this renderer runs on, for ash.
    #[cfg(feature = "raw-vulkan")]
    pub fn raw_handles(&self) -> RawVulkanHandles {
        self.raw.handles().into()
    }
}

/// Software CPU compositor for nested and headless targets.
pub struct CpuCompositor {
    raw: cxx::UniquePtr<ffi::CpuCompositorShim>,
}

impl CpuCompositor {
    pub fn new() -> Result<Self, Error> {
        let raw = ffi::cpu_compositor_new();
        if raw.is_null() {
            Err(Error::new(
                ErrorKind::Shell,
                "failed to create cpu compositor",
            ))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn composite_rect(&mut self, rect: Rect, color: Color) {
        self.raw.pin_mut().composite_rect(
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

    pub fn finish(&mut self, width: i32, height: i32, background: Color) -> Vec<u8> {
        self.raw.pin_mut().finish(
            width,
            height,
            background.r,
            background.g,
            background.b,
            background.a,
        )
    }
}

/// The `zwp_linux_dmabuf_v1` global. Borrows the [`Display`], and the
/// [`VulkanRenderer`] it advertises formats from and imports into.
pub struct LinuxDmabuf<'a> {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::LinuxDmabufShim>,
    _borrows: Holds<'a>,
}

impl<'a> LinuxDmabuf<'a> {
    pub fn new(display: &'a Display, renderer: Option<&'a VulkanRenderer>) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = if let Some(vk) = renderer {
            ffi::linux_dmabuf_new(&display.raw, &vk.raw, &mut err)
        } else {
            ffi::linux_dmabuf_new_headless(&display.raw, &mut err)
        };
        if raw.is_null() {
            Err(Error::new(ErrorKind::Gpu, err))
        } else {
            Ok(Self {
                raw,
                _borrows: Holds::new(),
            })
        }
    }

    /// Create `LinuxDmabuf` backed by the GPU of a [`SceneRenderer`]. If the
    /// renderer fell back to the CPU, a headless dmabuf manager is created.
    #[cfg(feature = "shell")]
    pub fn from_scene_renderer(
        display: &'a Display,
        renderer: &'a crate::gpu::SceneRenderer,
    ) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = crate::ffi_shell::linux_dmabuf_new_from_scene_renderer(
            &display.raw,
            &renderer.raw,
            &mut err,
        );
        if raw.is_null() {
            Err(Error::new(ErrorKind::Gpu, err))
        } else {
            Ok(Self {
                raw,
                _borrows: Holds::new(),
            })
        }
    }
}

/// The `wp_linux_drm_syncobj_manager_v1` global for explicit synchronization.
pub struct DrmSyncobjManager<'d> {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::DrmSyncobjManagerShim>,
    _display: Holds<'d>,
}

impl<'d> DrmSyncobjManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::drm_syncobj_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Gpu, err))
        } else {
            Ok(Self {
                raw,
                _display: Holds::new(),
            })
        }
    }
}

/// The screencopy protocol manager (`zwlr_screencopy_manager_v1` and `ext_image_copy_capture_manager_v1`).
pub struct ScreencopyManager<'d> {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::ScreencopyManagerShim>,
    _display: Holds<'d>,
}

impl<'d> ScreencopyManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::screencopy_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Gpu, err))
        } else {
            Ok(Self {
                raw,
                _display: Holds::new(),
            })
        }
    }
}
