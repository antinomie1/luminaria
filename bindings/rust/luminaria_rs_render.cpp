// luminaria_rs_render.cpp — C++ shim for luminaria.render.
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "luminaria/luminaria_rs.h"
#include "luminaria/luminaria_rs_render.h"
#include "luminaria/src/ffi.rs.h"
#include "luminaria/src/ffi_render.rs.h"

import luminaria.render;

#include "luminaria_rs_internal.h"

namespace lum {

bool drm_device_id_from_fd(int fd, DrmDeviceId& out) {
    auto id = lm::drm_device_id(fd);
    if (!id) return false;
    out = DrmDeviceId{id->major, id->minor};
    return true;
}

bool drm_device_id_from_path(rust::Str path, DrmDeviceId& out) {
    auto id = lm::drm_device_id(std::string(path));
    if (!id) return false;
    out = DrmDeviceId{id->major, id->minor};
    return true;
}

struct VulkanRendererShim::Impl {
    std::unique_ptr<lm::VulkanRenderer> renderer;
};

VulkanRendererShim::~VulkanRendererShim() = default;
VulkanRendererShim::VulkanRendererShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
VulkanRendererShim::Impl& VulkanRendererShim::impl() const { return *impl_; }

lm::VulkanRenderer* raw_ptr(const VulkanRendererShim& s) {
    return s.impl().renderer.get();
}

namespace {

lm::VulkanOptions vulkan_options(bool has_drm_device, const DrmDeviceId& drm_device) {
    lm::VulkanOptions opts{};
    if (has_drm_device) opts.drm_device = lm::DrmDeviceId{drm_device.major, drm_device.minor};
    return opts;
}

VulkanHandlesRaw handles_raw(const lm::VulkanRenderer& renderer) {
    const lm::VulkanHandles h = renderer.handles();
    VulkanHandlesRaw out{};
    out.instance = reinterpret_cast<std::uint64_t>(h.instance);
    out.physical_device = reinterpret_cast<std::uint64_t>(h.physical_device);
    out.device = reinterpret_cast<std::uint64_t>(h.device);
    out.queue = reinterpret_cast<std::uint64_t>(h.queue);
    out.queue_family = h.queue_family;
    out.api_version = h.api_version;
    for (const char* ext : h.device_extensions) out.device_extensions.push_back(rust::String(ext));
    return out;
}

} // namespace

std::unique_ptr<VulkanRendererShim> vulkan_renderer_new(bool has_drm_device, const DrmDeviceId& drm_device, rust::String& error) {
    try {
        const lm::VulkanOptions opts = vulkan_options(has_drm_device, drm_device);
        auto r = lm::VulkanRenderer::create(opts);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<VulkanRendererShim::Impl>();
        impl->renderer = std::make_unique<lm::VulkanRenderer>(std::move(*r));
        return std::make_unique<VulkanRendererShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

rust::Vec<std::uint64_t> VulkanRendererShim::scanout_modifiers(std::uint32_t drm_format) const {
    rust::Vec<std::uint64_t> out;
    if (impl_->renderer) {
        for (auto m : impl_->renderer->scanout_modifiers(drm_format)) {
            out.push_back(m);
        }
    }
    return out;
}

VulkanHandlesRaw VulkanRendererShim::handles() const {
    return impl_->renderer ? handles_raw(*impl_->renderer) : VulkanHandlesRaw{};
}

struct LinuxDmabufShim::Impl {
    std::optional<lm::LinuxDmabuf> dmabuf;
};
LinuxDmabufShim::~LinuxDmabufShim() = default;
LinuxDmabufShim::LinuxDmabufShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LinuxDmabufShim::Impl& LinuxDmabufShim::impl() const { return *impl_; }

std::unique_ptr<LinuxDmabufShim> linux_dmabuf_from_raw(lm::Display* disp, lm::VulkanRenderer* gpu, rust::String& error) {
    try {
        if (!disp) { error = "display not initialized"; return nullptr; }
        auto r = lm::LinuxDmabuf::create(*disp, gpu);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<LinuxDmabufShim::Impl>();
        impl->dmabuf.emplace(std::move(*r));
        return std::make_unique<LinuxDmabufShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::unique_ptr<LinuxDmabufShim> linux_dmabuf_new(const DisplayShim& display, const VulkanRendererShim& renderer, rust::String& error) {
    return linux_dmabuf_from_raw(raw_ptr(display), raw_ptr(renderer), error);
}

std::unique_ptr<LinuxDmabufShim> linux_dmabuf_new_headless(const DisplayShim& display, rust::String& error) {
    return linux_dmabuf_from_raw(raw_ptr(display), nullptr, error);
}

struct DrmSyncobjManagerShim::Impl { std::optional<lm::DrmSyncobjManager> mgr; };
DrmSyncobjManagerShim::~DrmSyncobjManagerShim() = default;
DrmSyncobjManagerShim::DrmSyncobjManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DrmSyncobjManagerShim::Impl& DrmSyncobjManagerShim::impl() const { return *impl_; }

std::unique_ptr<DrmSyncobjManagerShim> drm_syncobj_manager_new(const DisplayShim& display, rust::String& error) {
    try {
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        auto r = lm::DrmSyncobjManager::create(*disp);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<DrmSyncobjManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        return std::make_unique<DrmSyncobjManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}

struct ScreencopyManagerShim::Impl { std::optional<lm::ScreencopyManager> mgr; };
ScreencopyManagerShim::~ScreencopyManagerShim() = default;
ScreencopyManagerShim::ScreencopyManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ScreencopyManagerShim::Impl& ScreencopyManagerShim::impl() const { return *impl_; }

std::unique_ptr<ScreencopyManagerShim> screencopy_manager_new(const DisplayShim& display, rust::String& error) {
    try {
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        auto r = lm::ScreencopyManager::create(*disp);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<ScreencopyManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        return std::make_unique<ScreencopyManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}

bool screencopy_enable_toplevel_capture(ScreencopyManagerShim& mgr, const DisplayShim& display,
                                        std::size_t resolver, rust::String& error) {
    try {
        lm::Display* disp = raw_ptr(display);
        if (!disp || !mgr.impl().mgr || resolver == 0) { error = "display or resolver missing"; return false; }
        using Resolver = lm::Toplevel* (*)(wl_resource*);
        auto fn = reinterpret_cast<Resolver>(resolver);
        auto st = mgr.impl().mgr->enable_toplevel_capture(
            *disp, [fn](wl_resource* handle) { return fn(handle); });
        if (!st) { error = to_rust(st.error().message); return false; }
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

struct CpuCompositorShim::Impl {
    lm::CpuCompositor comp;
    std::vector<lm::CpuItem> items;
};

CpuCompositorShim::~CpuCompositorShim() = default;
CpuCompositorShim::CpuCompositorShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CpuCompositorShim::Impl& CpuCompositorShim::impl() const { return *impl_; }

std::unique_ptr<CpuCompositorShim> cpu_compositor_new() {
    return std::make_unique<CpuCompositorShim>(std::make_unique<CpuCompositorShim::Impl>());
}

void CpuCompositorShim::composite_rect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                                       float r, float g, float b, float a) {
    impl_->items.push_back(lm::RectFill{lm::Box{x, y, width, height}, lm::Color{r, g, b, a}});
}

rust::Vec<std::uint8_t> CpuCompositorShim::finish(std::int32_t width, std::int32_t height,
                                                  float bg_r, float bg_g, float bg_b, float bg_a) {
    rust::Vec<std::uint8_t> out;
    impl_->comp.composite(width, height, lm::Color{bg_r, bg_g, bg_b, bg_a}, impl_->items);
    impl_->items.clear();
    const auto& px = impl_->comp.pixels();
    const auto* ptr = reinterpret_cast<const uint8_t*>(px.data());
    size_t bytes = px.size() * sizeof(lm::Pixel);
    out.reserve(bytes);
    for (size_t i = 0; i < bytes; ++i) out.push_back(ptr[i]);
    return out;
}

} // namespace lum
