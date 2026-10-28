// luminaria_rs_render.h — C++ shims for luminaria.render.
#pragma once

#include <cstdint>
#include <memory>

#include "luminaria/luminaria_rs.h"

namespace luminaria {
class Display;
class VulkanRenderer;
}

namespace lum {

class VulkanRendererShim;
luminaria::VulkanRenderer* raw_ptr(const VulkanRendererShim& s);

struct DrmDeviceId;
struct VulkanHandlesRaw;

bool drm_device_id_from_fd(int fd, DrmDeviceId& out);
bool drm_device_id_from_path(rust::Str path, DrmDeviceId& out);

class VulkanRendererShim {
public:
    ~VulkanRendererShim();
    VulkanRendererShim(const VulkanRendererShim&) = delete;
    VulkanRendererShim& operator=(const VulkanRendererShim&) = delete;

    rust::Vec<std::uint64_t> scanout_modifiers(std::uint32_t drm_format) const;
    VulkanHandlesRaw handles() const;

    struct Impl;
    Impl& impl() const;
    explicit VulkanRendererShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<VulkanRendererShim> vulkan_renderer_new(bool has_drm_device, const DrmDeviceId& drm_device, rust::String& error);

class CpuCompositorShim {
public:
    ~CpuCompositorShim();
    CpuCompositorShim(const CpuCompositorShim&) = delete;
    CpuCompositorShim& operator=(const CpuCompositorShim&) = delete;

    void composite_rect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                        float r, float g, float b, float a);
    rust::Vec<std::uint8_t> finish(std::int32_t width, std::int32_t height,
                                   float bg_r, float bg_g, float bg_b, float bg_a);

    struct Impl;
    Impl& impl() const;
    explicit CpuCompositorShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<CpuCompositorShim> cpu_compositor_new();

class LinuxDmabufShim {
public:
    ~LinuxDmabufShim();
    LinuxDmabufShim(const LinuxDmabufShim&) = delete;
    LinuxDmabufShim& operator=(const LinuxDmabufShim&) = delete;

    struct Impl;
    Impl& impl() const;
    explicit LinuxDmabufShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<LinuxDmabufShim> linux_dmabuf_new(const DisplayShim& display, const VulkanRendererShim& renderer, rust::String& error);
std::unique_ptr<LinuxDmabufShim> linux_dmabuf_new_headless(const DisplayShim& display, rust::String& error);
std::unique_ptr<LinuxDmabufShim> linux_dmabuf_from_raw(luminaria::Display* disp, luminaria::VulkanRenderer* gpu, rust::String& error);

class DrmSyncobjManagerShim {
public:
    ~DrmSyncobjManagerShim();
    DrmSyncobjManagerShim(const DrmSyncobjManagerShim&) = delete;
    DrmSyncobjManagerShim& operator=(const DrmSyncobjManagerShim&) = delete;

    struct Impl;
    Impl& impl() const;
    explicit DrmSyncobjManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<DrmSyncobjManagerShim> drm_syncobj_manager_new(const DisplayShim& display, rust::String& error);

class ScreencopyManagerShim {
public:
    ~ScreencopyManagerShim();
    ScreencopyManagerShim(const ScreencopyManagerShim&) = delete;
    ScreencopyManagerShim& operator=(const ScreencopyManagerShim&) = delete;

    struct Impl;
    Impl& impl() const;
    explicit ScreencopyManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<ScreencopyManagerShim> screencopy_manager_new(const DisplayShim& display, rust::String& error);
bool screencopy_enable_toplevel_capture(ScreencopyManagerShim& mgr, const DisplayShim& display,
                                        std::size_t resolver, rust::String& error);
void screencopy_set_renderer(ScreencopyManagerShim& mgr, const VulkanRendererShim& renderer);

} // namespace lum
