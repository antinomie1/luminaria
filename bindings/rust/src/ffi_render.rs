//! The render bridge: `import luminaria.render;` on the C++ side.
//!
//! Cargo feature `render`. Everything here needs Vulkan, GBM or libdrm.
//! A consumer that leaves the feature off never compiles this bridge, never
//! builds `luminaria_rs_render.cpp` and never links them.

#[cxx::bridge(namespace = "lum")]
pub(crate) mod ffi {
    /// A DRM device node's identity: `st_rdev` of /dev/dri/cardN or renderDN.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    struct DrmDeviceId {
        major: u32,
        minor: u32,
    }

    #[allow(dead_code)]
    #[derive(Debug, Clone, Default)]
    struct VulkanHandlesRaw {
        instance: u64,
        physical_device: u64,
        device: u64,
        queue: u64,
        queue_family: u32,
        api_version: u32,
        device_extensions: Vec<String>,
    }

    unsafe extern "C++" {
        include!("luminaria/luminaria_rs.h");
        include!("luminaria/luminaria_rs_render.h");
        include!("luminaria/src/ffi.rs.h");

        type DisplayShim = crate::ffi::DisplayShim;
        type OutputShim = crate::ffi::OutputShim;

        fn drm_device_id_from_fd(fd: i32, out: &mut DrmDeviceId) -> bool;
        fn drm_device_id_from_path(path: &str, out: &mut DrmDeviceId) -> bool;

        type VulkanRendererShim;
        fn vulkan_renderer_new(
            has_drm_device: bool,
            drm_device: &DrmDeviceId,
            error: &mut String,
        ) -> UniquePtr<VulkanRendererShim>;
        fn scanout_modifiers(self: &VulkanRendererShim, drm_format: u32) -> Vec<u64>;
        #[allow(dead_code)]
        fn handles(self: &VulkanRendererShim) -> VulkanHandlesRaw;

        type CpuCompositorShim;
        fn cpu_compositor_new() -> UniquePtr<CpuCompositorShim>;
        fn composite_rect(
            self: Pin<&mut CpuCompositorShim>,
            x: i32,
            y: i32,
            width: i32,
            height: i32,
            r: f32,
            g: f32,
            b: f32,
            a: f32,
        );
        fn finish(
            self: Pin<&mut CpuCompositorShim>,
            width: i32,
            height: i32,
            bg_r: f32,
            bg_g: f32,
            bg_b: f32,
            bg_a: f32,
        ) -> Vec<u8>;

        type LinuxDmabufShim;
        fn linux_dmabuf_new(
            display: &DisplayShim,
            renderer: &VulkanRendererShim,
            error: &mut String,
        ) -> UniquePtr<LinuxDmabufShim>;
        fn linux_dmabuf_new_headless(
            display: &DisplayShim,
            error: &mut String,
        ) -> UniquePtr<LinuxDmabufShim>;

        type DrmSyncobjManagerShim;
        fn drm_syncobj_manager_new(
            display: &DisplayShim,
            error: &mut String,
        ) -> UniquePtr<DrmSyncobjManagerShim>;

        type ScreencopyManagerShim;
        fn screencopy_manager_new(
            display: &DisplayShim,
            error: &mut String,
        ) -> UniquePtr<ScreencopyManagerShim>;
    }
}
