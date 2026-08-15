// luminaria/render/vulkan.cppm — Vulkan renderer (Vulkan-Hpp RAII under the hood).
//
// Importing luminaria pulls in no Vulkan headers: all vk::raii state hides
// behind Impl, and vulkan_raii.hpp stays in the implementation unit.
// Vulkan-Hpp uses exceptions internally; they are caught at every method
// boundary and turned into Result, so nothing throws across a C callback.

module;

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <unistd.h> // dup
#include <drm_fourcc.h>
#include <vulkan/vulkan_raii.hpp>
#include "quad_frag_spv.h"
#include "quad_vert_spv.h"

export module luminaria.gpu:vulkan;

import luminaria;

export namespace luminaria {

/// A surface's pixels placed at (x,y). `rgba` is w*h*4 tightly-packed RGBA8.
struct TextureFill {
    int x, y, w, h;
    const std::uint8_t* rgba;
};

/// A GPU-resident image the renderer composites from: either a client dmabuf
/// imported with no copy at all, or CPU pixels uploaded once. Move-only, and it
/// must not outlive the VulkanRenderer that made it.
class GpuTexture {
public:
    ~GpuTexture();
    GpuTexture(GpuTexture&&) noexcept;
    GpuTexture& operator=(GpuTexture&&) noexcept;
    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;

    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;

    struct Impl;

private:
    friend class VulkanRenderer;
    std::unique_ptr<Impl> impl_;
    explicit GpuTexture(std::unique_ptr<Impl> impl) noexcept;
};

/// A GPU render target that is also a dmabuf, so KMS can scan it out directly:
/// the compositing result never touches system memory. Move-only, and it must
/// not outlive the VulkanRenderer that made it.
class ScanoutTarget {
public:
    ~ScanoutTarget();
    ScanoutTarget(ScanoutTarget&&) noexcept;
    ScanoutTarget& operator=(ScanoutTarget&&) noexcept;
    ScanoutTarget(const ScanoutTarget&) = delete;
    ScanoutTarget& operator=(const ScanoutTarget&) = delete;

    /// The exported plane. `fd` is owned by this target — borrow, don't close.
    [[nodiscard]] const DmabufPlane& plane() const noexcept;

    /// A sync_file the GPU must wait for before drawing into this target again:
    /// the KMS out-fence of the flip that is still scanning it out. Takes
    /// ownership (-1 is a no-op); the next render_to waits on it and closes it.
    /// Without this the caller has to keep the display and the GPU in step by
    /// blocking, which is the stall explicit sync exists to remove.
    void set_acquire_fence(int fd) noexcept;
    [[nodiscard]] int width() const noexcept { return plane().width; }
    [[nodiscard]] int height() const noexcept { return plane().height; }

    struct Impl;

private:
    friend class VulkanRenderer;
    std::unique_ptr<Impl> impl_;
    explicit ScanoutTarget(std::unique_ptr<Impl> impl) noexcept;
};

/// A GpuTexture stretched into the destination rect (x,y,w,h), in the output's
/// LOGICAL coordinates — the renderer applies scale and rotation.
struct GpuTextureFill {
    const GpuTexture* texture;
    int x, y, w, h;

    /// How the source buffer is oriented relative to the surface — pass
    /// `Surface::buffer_transform()`. Folded into the output's own rotation so
    /// a rotated client on a rotated screen is still one sample.
    Transform transform = Transform::normal;

    /// The part of the destination the caller guarantees is fully opaque, in
    /// logical coordinates. Whatever is behind it is not drawn at all.
    /// Empty (the default) means "assume translucent".
    ///
    /// A set of boxes and not one box, because the difference is visible: a
    /// window with rounded corners declares itself opaque everywhere *except*
    /// the corners, and a bounding box would claim those corners too — culling
    /// the wallpaper that should show through them.
    ///
    /// Borrowed, not owned: this is filled once per frame per surface, and a
    /// `Region` here would be a heap allocation per surface per frame. The
    /// shell layer points it at its own arena; the boxes must outlive the
    /// `render_to` call and must not overlap each other.
    std::span<const Box> opaque{};

    /// Source crop, normalized against the BUFFER (before `transform`).
    /// Defaults to the whole texture; wp_viewporter narrows it.
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
};

/// How an output's logical coordinates map onto its framebuffer: the integer
/// scale (2 for HiDPI) and the rotation/reflection of the panel.
struct OutputMapping {
    Transform transform = Transform::normal;
    int scale = 1;
};

/// GPU-side synchronisation for one render: fences in, a fence out, no CPU
/// stall in between. This is what makes explicit sync worth having — a client's
/// acquire point becomes a semaphore the GPU waits on, and the finished render
/// becomes a fence KMS waits on (`IN_FENCE_FD` on the atomic commit).
struct RenderSync {
    /// sync_file fds the render must wait for before sampling — a client's
    /// linux-drm-syncobj acquire point. Borrowed; the caller keeps ownership.
    std::span<const int> wait_fds{};

    /// If non-null, receives a sync_file fd that signals when the render is
    /// done, and render_to returns without waiting. The caller OWNS it: hand it
    /// to `Output::commit_scanout()`, or close it. Left null, render_to blocks
    /// on a fence as before, which is what the CPU read-back paths need.
    int* out_fence_fd = nullptr;
};

class VulkanRenderer {
public:
    /// Bring up an instance + device. Fails if no Vulkan device is available.
    [[nodiscard]] static Result<VulkanRenderer> create();

    ~VulkanRenderer();
    VulkanRenderer(VulkanRenderer&&) noexcept;
    VulkanRenderer& operator=(VulkanRenderer&&) noexcept;
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    /// Render a solid-color frame offscreen and read back pixel (0,0). Proves the
    /// GPU path end to end; real surface compositing arrives in Phase 2.
    [[nodiscard]] Result<Pixel> render_clear_readback(int width, int height, Color color);

    /// Composite: clear to `background`, then paint each RectFill (back-to-front)
    /// into a width×height frame. Returns the frame as row-major RGBA pixels.
    [[nodiscard]] Result<std::vector<Pixel>> render_rects(int width, int height, Color background,
                                                          std::span<const RectFill> rects);

    /// Composite rects, then blit surface textures (opaque, 1:1) on top at their
    /// positions. `rgba` is tightly-packed w×h RGBA8. Returns row-major RGBA.
    ///
    /// Copy-based placement: no scaling, no rotation, no alpha blending. This is
    /// the fallback for backends that cannot scan out a dmabuf (headless,
    /// nested) and for screencopy read-back. Anything that can use `render_to`
    /// should — that one is the real pipeline.
    [[nodiscard]] Result<std::vector<Pixel>> composite(int width, int height, Color background,
                                                       std::span<const RectFill> rects,
                                                       std::span<const TextureFill> textures);

    // --- GPU compositing path (no CPU read-back anywhere) ---
    //
    // import_texture(client dmabuf) / upload_texture(shm pixels)
    //   -> render_to(scanout target)
    //   -> Output::import_scanout + commit_scanout (KMS atomic page-flip).

    /// Import a client dmabuf as a sampled texture. Nothing is copied: the GPU
    /// reads the client's pages directly. `plane.fd` is borrowed (dup'd inside).
    [[nodiscard]] Result<GpuTexture> import_texture(const DmabufPlane& plane);

    /// Upload tightly-packed RGBA8 into a device-local texture (the wl_shm path).
    [[nodiscard]] Result<GpuTexture> upload_texture(int width, int height,
                                                     std::span<const std::uint8_t> rgba);

    /// DRM modifiers this GPU can render into *and* export as a dmabuf for
    /// `drm_format`. Intersect with what the display hardware can scan out.
    [[nodiscard]] std::vector<std::uint64_t> scanout_modifiers(std::uint32_t drm_format);

    /// Allocate a render target exported as a dmabuf. The driver picks one of
    /// `modifiers` (pass the intersection of scanout_modifiers() and what the
    /// output accepts); an empty list means LINEAR only.
    [[nodiscard]] Result<ScanoutTarget> create_scanout(int width, int height,
                                                        std::uint32_t drm_format,
                                                        std::span<const std::uint64_t> modifiers);

    /// Composite straight into a scanout target: clear to `background`, paint the
    /// rects, then draw each texture into its destination rect (scaling and
    /// rotating as needed), back to front.
    ///
    /// Rects, textures and damage are all in the output's logical coordinates;
    /// `mapping` turns those into framebuffer pixels, so a rotated or HiDPI
    /// output needs no special-casing from the caller.
    ///
    /// `damage` limits the work to the regions that actually changed, in logical
    /// coordinates; empty means "redraw everything". It is turned into a set of
    /// disjoint boxes and each is scissored separately — two small dirty corners
    /// cost two small scissors, not the rectangle spanning them. The rest of the
    /// target keeps the pixels it already had, so with N buffers in rotation the
    /// caller must pass the union of the last N frames' damage — this target's
    /// content is N frames old, not one.
    ///
    /// Anything hidden behind a `GpuTextureFill::opaque` rect is skipped.
    ///
    /// `sync` decides how this call joins up with the GPU: by default it blocks
    /// on a fence and the caller may page-flip on return. Pass wait fds and ask
    /// for an out-fence instead and nothing waits on the CPU at all — see
    /// RenderSync.
    [[nodiscard]] Status render_to(ScanoutTarget& target, Color background,
                                   std::span<const RectFill> rects,
                                   std::span<const GpuTextureFill> textures,
                                   std::span<const Box> damage = {},
                                   const OutputMapping& mapping = {},
                                   const RenderSync& sync = {});

    // --- linux-dmabuf import/export (any DRM modifier the GPU supports) ---

    /// True if the device exposes the external-memory-dmabuf + DRM-modifier
    /// extensions, i.e. import/export below actually work.
    [[nodiscard]] bool dmabuf_supported() const noexcept;

    /// DRM modifiers usable for import+export of `drm_format` (single-plane,
    /// transfer-capable). Always includes DRM_FORMAT_MOD_LINEAR when supported.
    /// Empty if dmabuf is unsupported or the format has no usable modifier.
    [[nodiscard]] std::vector<std::uint64_t> dmabuf_modifiers(std::uint32_t drm_format);

    /// Read a scanout target back as tightly-packed RGBA8, for the backends that
    /// present CPU pixels (headless, nested wl_shm) and for screencopy.
    ///
    /// Copies from the target's own VkImage — the renderer already owns it, so
    /// nothing is imported — through a staging buffer the target keeps and
    /// reuses, in host-CACHED memory where the driver has any. Re-importing the
    /// dmabuf and reading uncached memory every frame instead costs ~90ms on a
    /// 800x600 frame, which is the difference between a smooth pointer and a
    /// visibly lagging one.
    ///
    /// `out` is resized to width*height*4.
    [[nodiscard]] Status read_scanout(ScanoutTarget& target, std::vector<std::uint8_t>& out);

    /// Import a single-plane dmabuf (ARGB8888/XRGB8888, any modifier) and read it
    /// back as tightly-packed RGBA8. `fd` is borrowed (dup'd internally). For a
    /// target this renderer made, prefer `read_scanout()` — it is far cheaper.
    [[nodiscard]] Result<std::vector<std::uint8_t>> import_dmabuf(int fd, int width, int height,
                                                                  std::uint32_t drm_format,
                                                                  std::uint32_t offset,
                                                                  std::uint32_t stride,
                                                                  std::uint64_t modifier);

    /// Write tightly-packed RGBA8 into a single-plane dmabuf target (screencopy).
    /// `fd` is borrowed (dup'd internally).
    [[nodiscard]] Status export_dmabuf(int fd, int width, int height, std::uint32_t drm_format,
                                       std::uint32_t offset, std::uint32_t stride,
                                       std::uint64_t modifier,
                                       const std::vector<std::uint8_t>& rgba);

private:
    // A GpuTexture caches a descriptor set the renderer owns, and hands it back
    // when it dies; that needs to name the renderer's Impl.
    friend class GpuTexture;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit VulkanRenderer(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

/// The textured-quad pipeline: one draw per surface, positioned by push
/// constants. Built once per target format and kept — compiling a pipeline every
/// frame would cost more than the frame.
struct QuadPipeline {
    vk::Format format;
    vk::raii::PipelineLayout layout;
    vk::raii::RenderPass load_pass;  // partial repaint: keep what is there
    vk::raii::RenderPass clear_pass; // full repaint: contents are undefined
    vk::raii::Pipeline pipeline;
};

/// One submitted-but-not-waited-for render. Without the fence stall the command
/// buffer, its descriptors and its semaphores are still in use after render_to
/// returns, so they live here until the fence says otherwise.
struct InFlight {
    vk::raii::Fence fence;
    vk::raii::CommandBuffers cmds;
    vk::raii::Framebuffer framebuffer;
    std::vector<vk::raii::Semaphore> semaphores;
    std::uint64_t index = 0; ///< submission order, for retiring descriptor sets
};

/// A descriptor set whose texture is gone. It cannot go back on the free list
/// until every submit that might still be reading it has finished, which is
/// what `after` records.
struct RetiredSet {
    vk::DescriptorSet set;
    std::uint64_t after;
};

struct VulkanRenderer::Impl {
    vk::raii::Context context;
    vk::raii::Instance instance;
    vk::raii::PhysicalDevice physical;
    std::uint32_t queue_family;
    vk::raii::Device device;
    vk::raii::Queue queue;
    vk::raii::CommandPool command_pool;
    bool dmabuf_ok = false;      // external-memory-dmabuf + DRM-modifier available
    bool queue_foreign = false;  // VK_EXT_queue_family_foreign available
    bool sync_fd_ok = false;     // VK_KHR_external_semaphore_fd: fences in and out
    std::optional<vk::raii::Sampler> sampler;
    std::vector<QuadPipeline> pipelines;
    std::vector<InFlight> in_flight;
    std::uint64_t submits = 0;

    // --- descriptor sets, cached per texture ---
    //
    // Every quad binds one combined image sampler, and the binding for a given
    // texture never changes: the image and its view are created once and live
    // as long as the texture does. So the set is written once, when the texture
    // is first drawn, and simply re-bound afterwards. What used to happen was a
    // fresh pool per frame plus one allocate-and-write per *fill*, which for a
    // screen full of unchanging windows is the same descriptor rewritten sixty
    // times a second.
    //
    // The layout is shared rather than per-pipeline: it is the same single
    // binding whatever the target format, and one layout means one free list.
    std::optional<vk::raii::DescriptorSetLayout> set_layout;
    std::vector<vk::raii::DescriptorPool> set_pools;
    std::vector<vk::DescriptorSet> free_sets;
    std::vector<RetiredSet> retiring;
    std::uint32_t sets_in_pool = 0; ///< allocated out of set_pools.back()

    QuadPipeline& quad_pipeline(vk::Format format);
    vk::DescriptorSetLayout quad_set_layout();
    vk::DescriptorSet acquire_set();

    /// A texture is gone; its set can be reused once nothing is reading it.
    void retire_set(vk::DescriptorSet set) {
        if (set) {
            retiring.push_back(RetiredSet{set, submits});
        }
    }

    /// Drop everything the GPU has finished with. Cheap and called once per
    /// render; the list is bounded by how many frames the display is behind.
    void reap() {
        std::erase_if(in_flight,
                      [](const InFlight& f) { return f.fence.getStatus() == vk::Result::eSuccess; });
        // A retired set is safe to rewrite once every submit that could still
        // reference it has completed. in_flight is in submission order, so the
        // oldest survivor is the whole test.
        const std::uint64_t oldest =
            in_flight.empty() ? submits + 1 : in_flight.front().index;
        std::erase_if(retiring, [&](const RetiredSet& r) {
            if (r.after >= oldest) {
                return false;
            }
            free_sets.push_back(r.set);
            return true;
        });
    }
};

namespace {

// First queue family that can clear an image (graphics implies transfer+clear).
std::uint32_t pick_graphics_family(const vk::raii::PhysicalDevice& phys) {
    const auto families = phys.getQueueFamilyProperties();
    for (std::uint32_t i = 0; i < families.size(); ++i) {
        if (families[i].queueFlags & vk::QueueFlagBits::eGraphics) {
            return i;
        }
    }
    throw std::runtime_error("no graphics queue family");
}

std::uint32_t find_memory_type(const vk::raii::PhysicalDevice& phys, std::uint32_t type_bits,
                               vk::MemoryPropertyFlags want) {
    const auto props = phys.getMemoryProperties();
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    throw std::runtime_error("no suitable memory type");
}

} // namespace

VulkanRenderer::VulkanRenderer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
VulkanRenderer::~VulkanRenderer() {
    // Renders submitted without a CPU wait may still be running. Tearing down
    // the device out from under them is undefined behaviour, so drain first.
    if (impl_ && !impl_->in_flight.empty()) {
        impl_->queue.waitIdle();
        impl_->in_flight.clear();
    }
}
VulkanRenderer::VulkanRenderer(VulkanRenderer&&) noexcept = default;
VulkanRenderer& VulkanRenderer::operator=(VulkanRenderer&&) noexcept = default;

Result<VulkanRenderer> VulkanRenderer::create() {
    try {
        vk::raii::Context context;

        vk::ApplicationInfo app{"luminaria", 1, "luminaria", 1, VK_API_VERSION_1_1};
        vk::InstanceCreateInfo instance_info{{}, &app};
        vk::raii::Instance instance{context, instance_info};

        vk::raii::PhysicalDevices devices{instance};
        if (devices.empty()) {
            return fail("no Vulkan physical device");
        }
        vk::raii::PhysicalDevice physical = std::move(devices.front());

        const std::uint32_t queue_family = pick_graphics_family(physical);
        const float priority = 1.0f;
        vk::DeviceQueueCreateInfo queue_info{{}, queue_family, 1, &priority};

        // Enable dmabuf import/export extensions when the device has them. If any
        // required one is missing, the renderer still works (shm path); only
        // import/export report unsupported.
        const auto avail = physical.enumerateDeviceExtensionProperties();
        auto has = [&](const char* name) {
            return std::any_of(avail.begin(), avail.end(), [&](const auto& e) {
                return std::strcmp(e.extensionName, name) == 0;
            });
        };
        std::vector<const char*> exts;
        const std::array<const char*, 4> required{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                                  VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                                                  VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
                                                  VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME};
        bool dmabuf_ok = true;
        for (const char* e : required) {
            if (has(e)) {
                exts.push_back(e);
            } else {
                dmabuf_ok = false;
            }
        }
        bool queue_foreign = has(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
        if (dmabuf_ok && queue_foreign) {
            exts.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
        }
        if (!dmabuf_ok) {
            exts.clear(); // don't half-enable; nothing else needs these
            queue_foreign = false;
        }
        // Semaphores that cross the device boundary as sync_file fds: a client's
        // acquire point comes in, the finished render goes out to KMS. Optional —
        // without it render_to falls back to a CPU fence wait.
        const bool sync_fd_ok = has(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
        if (sync_fd_ok) {
            exts.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
        }

        vk::DeviceCreateInfo device_info{{}, queue_info};
        device_info.setPEnabledExtensionNames(exts);
        vk::raii::Device device{physical, device_info};
        vk::raii::Queue queue{device, queue_family, 0};
        vk::raii::CommandPool command_pool{
            device, vk::CommandPoolCreateInfo{vk::CommandPoolCreateFlagBits::eTransient, queue_family}};

        auto impl = std::make_unique<Impl>(Impl{std::move(context), std::move(instance),
                                                std::move(physical), queue_family, std::move(device),
                                                std::move(queue), std::move(command_pool),
                                                dmabuf_ok, queue_foreign, sync_fd_ok, std::nullopt,
                                                {}, {}});
        // Linear filtering with clamped edges: a scaled or rotated surface must
        // not smear its opposite edge in along the seam.
        impl->sampler.emplace(
            impl->device,
            vk::SamplerCreateInfo{{},
                                  vk::Filter::eLinear,
                                  vk::Filter::eLinear,
                                  vk::SamplerMipmapMode::eNearest,
                                  vk::SamplerAddressMode::eClampToEdge,
                                  vk::SamplerAddressMode::eClampToEdge,
                                  vk::SamplerAddressMode::eClampToEdge});
        return VulkanRenderer{std::move(impl)};
    } catch (const std::exception& e) {
        return fail(std::string{"vulkan init: "} + e.what());
    }
}

Result<Pixel> VulkanRenderer::render_clear_readback(int width, int height, Color color) {
    if (width <= 0 || height <= 0) {
        return fail("invalid dimensions");
    }
    try {
        auto& device = impl_->device;

        // Host-visible LINEAR image: clear it on the GPU, then map and read directly.
        const vk::ImageSubresourceRange whole{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        vk::ImageCreateInfo image_info{
            {},
            vk::ImageType::e2D,
            vk::Format::eR8G8B8A8Unorm,
            vk::Extent3D{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1},
            1,
            1,
            vk::SampleCountFlagBits::e1,
            vk::ImageTiling::eLinear,
            vk::ImageUsageFlagBits::eTransferDst,
            vk::SharingMode::eExclusive,
            {},
            vk::ImageLayout::eUndefined};
        vk::raii::Image image{device, image_info};

        const auto req = image.getMemoryRequirements();
        const std::uint32_t mem_type = find_memory_type(
            impl_->physical, req.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        vk::raii::DeviceMemory memory{device, vk::MemoryAllocateInfo{req.size, mem_type}};
        image.bindMemory(*memory, 0);

        vk::raii::CommandBuffers command_buffers{
            device, vk::CommandBufferAllocateInfo{*impl_->command_pool,
                                                  vk::CommandBufferLevel::ePrimary, 1}};
        vk::raii::CommandBuffer& cmd = command_buffers.front();

        cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        vk::ImageMemoryBarrier to_general{
            {},
            vk::AccessFlagBits::eTransferWrite,
            vk::ImageLayout::eUndefined,
            vk::ImageLayout::eGeneral,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            *image,
            whole};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_general);

        vk::ClearColorValue clear{std::array<float, 4>{color.r, color.g, color.b, color.a}};
        cmd.clearColorImage(*image, vk::ImageLayout::eGeneral, clear, whole);

        vk::ImageMemoryBarrier to_host{
            vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eHostRead,
            vk::ImageLayout::eGeneral,
            vk::ImageLayout::eGeneral,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            *image,
            whole};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                            {}, {}, {}, to_host);
        cmd.end();

        vk::raii::Fence fence{device, vk::FenceCreateInfo{}};
        vk::SubmitInfo submit;
        submit.setCommandBuffers(*cmd);
        impl_->queue.submit(submit, *fence);
        if (device.waitForFences(*fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()) !=
            vk::Result::eSuccess) {
            return fail("fence wait failed");
        }

        const auto layout = image.getSubresourceLayout(
            vk::ImageSubresource{vk::ImageAspectFlagBits::eColor, 0, 0});
        auto* base = static_cast<std::uint8_t*>(memory.mapMemory(0, VK_WHOLE_SIZE));
        auto* px = base + layout.offset;
        Pixel out{px[0], px[1], px[2], px[3]};
        memory.unmapMemory();
        return out;
    } catch (const std::exception& e) {
        return fail(std::string{"vulkan render: "} + e.what());
    }
}

Result<std::vector<Pixel>> VulkanRenderer::render_rects(int width, int height, Color background,
                                                       std::span<const RectFill> rects) {
    return composite(width, height, background, rects, {});
}

Result<std::vector<Pixel>> VulkanRenderer::composite(int width, int height, Color background,
                                                    std::span<const RectFill> rects,
                                                    std::span<const TextureFill> textures) {
    if (width <= 0 || height <= 0) {
        return fail("invalid dimensions");
    }
    try {
        auto& device = impl_->device;
        const auto uw = static_cast<uint32_t>(width);
        const auto uh = static_cast<uint32_t>(height);
        const vk::Format format = vk::Format::eR8G8B8A8Unorm;

        // Color-attachment image we render into, then copy out.
        vk::raii::Image image{
            device,
            vk::ImageCreateInfo{{},
                                vk::ImageType::e2D,
                                format,
                                vk::Extent3D{uw, uh, 1},
                                1,
                                1,
                                vk::SampleCountFlagBits::e1,
                                vk::ImageTiling::eOptimal,
                                vk::ImageUsageFlagBits::eColorAttachment |
                                    vk::ImageUsageFlagBits::eTransferSrc |
                                    vk::ImageUsageFlagBits::eTransferDst,
                                vk::SharingMode::eExclusive,
                                {},
                                vk::ImageLayout::eUndefined}};
        const auto img_req = image.getMemoryRequirements();
        vk::raii::DeviceMemory image_memory{
            device, vk::MemoryAllocateInfo{img_req.size,
                                           find_memory_type(impl_->physical, img_req.memoryTypeBits,
                                                            vk::MemoryPropertyFlagBits::eDeviceLocal)}};
        image.bindMemory(*image_memory, 0);

        vk::raii::ImageView view{
            device, vk::ImageViewCreateInfo{{},
                                            *image,
                                            vk::ImageViewType::e2D,
                                            format,
                                            {},
                                            {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}};

        vk::AttachmentDescription attachment{{},
                                             format,
                                             vk::SampleCountFlagBits::e1,
                                             vk::AttachmentLoadOp::eClear,
                                             vk::AttachmentStoreOp::eStore,
                                             vk::AttachmentLoadOp::eDontCare,
                                             vk::AttachmentStoreOp::eDontCare,
                                             vk::ImageLayout::eUndefined,
                                             vk::ImageLayout::eTransferDstOptimal};
        vk::AttachmentReference color_ref{0, vk::ImageLayout::eColorAttachmentOptimal};
        vk::SubpassDescription subpass{{}, vk::PipelineBindPoint::eGraphics, {}, color_ref};
        vk::raii::RenderPass render_pass{device,
                                         vk::RenderPassCreateInfo{{}, attachment, subpass}};

        vk::ImageView fb_attachment = *view;
        vk::raii::Framebuffer framebuffer{
            device, vk::FramebufferCreateInfo{{}, *render_pass, fb_attachment, uw, uh, 1}};

        // Host-visible buffer to read the frame back.
        const vk::DeviceSize buffer_size = static_cast<vk::DeviceSize>(uw) * uh * 4;
        vk::raii::Buffer readback{device, vk::BufferCreateInfo{{}, buffer_size,
                                                               vk::BufferUsageFlagBits::eTransferDst,
                                                               vk::SharingMode::eExclusive}};
        const auto buf_req = readback.getMemoryRequirements();
        vk::raii::DeviceMemory readback_memory{
            device, vk::MemoryAllocateInfo{
                        buf_req.size,
                        find_memory_type(impl_->physical, buf_req.memoryTypeBits,
                                         vk::MemoryPropertyFlagBits::eHostVisible |
                                             vk::MemoryPropertyFlagBits::eHostCoherent)}};
        readback.bindMemory(*readback_memory, 0);

        vk::raii::CommandBuffers command_buffers{
            device, vk::CommandBufferAllocateInfo{*impl_->command_pool,
                                                  vk::CommandBufferLevel::ePrimary, 1}};
        vk::raii::CommandBuffer& cmd = command_buffers.front();
        cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

        vk::ClearValue clear{vk::ClearColorValue{
            std::array<float, 4>{background.r, background.g, background.b, background.a}}};
        cmd.beginRenderPass(vk::RenderPassBeginInfo{*render_pass, *framebuffer,
                                                    vk::Rect2D{{0, 0}, {uw, uh}}, clear},
                            vk::SubpassContents::eInline);
        for (const RectFill& rf : rects) {
            const int x0 = std::max(0, rf.box.x);
            const int y0 = std::max(0, rf.box.y);
            const int x1 = std::min(width, rf.box.x + rf.box.width);
            const int y1 = std::min(height, rf.box.y + rf.box.height);
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }
            vk::ClearAttachment clear_att{
                vk::ImageAspectFlagBits::eColor, 0,
                vk::ClearColorValue{
                    std::array<float, 4>{rf.color.r, rf.color.g, rf.color.b, rf.color.a}}};
            vk::ClearRect clear_rect{
                vk::Rect2D{{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}},
                0, 1};
            cmd.clearAttachments(clear_att, clear_rect);
        }
        cmd.endRenderPass();
        // The render pass leaves the image in TransferDstOptimal, ready for the
        // texture copies below. Staging buffers must outlive the submit.
        std::vector<vk::raii::Buffer> staging;
        std::vector<vk::raii::DeviceMemory> staging_memory;
        for (const TextureFill& t : textures) {
            if (t.rgba == nullptr || t.w <= 0 || t.h <= 0) {
                continue;
            }
            // Clip the surface rect to the output; copy only the visible sub-rect
            // (a partially off-screen window still shows the part that's on-screen).
            const int dx0 = std::max(0, t.x);
            const int dy0 = std::max(0, t.y);
            const int dx1 = std::min(width, t.x + t.w);
            const int dy1 = std::min(height, t.y + t.h);
            if (dx1 <= dx0 || dy1 <= dy0) {
                continue; // fully off-screen
            }
            const int cw = dx1 - dx0;
            const int ch = dy1 - dy0;
            const int sx = dx0 - t.x; // source-column offset into t.rgba
            const int sy = dy0 - t.y; // source-row offset into t.rgba

            const vk::DeviceSize size = static_cast<vk::DeviceSize>(cw) * ch * 4;
            vk::raii::Buffer sb{device, vk::BufferCreateInfo{{}, size,
                                                             vk::BufferUsageFlagBits::eTransferSrc,
                                                             vk::SharingMode::eExclusive}};
            const auto sreq = sb.getMemoryRequirements();
            vk::raii::DeviceMemory sm{
                device, vk::MemoryAllocateInfo{
                            sreq.size,
                            find_memory_type(impl_->physical, sreq.memoryTypeBits,
                                             vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent)}};
            sb.bindMemory(*sm, 0);
            auto* dstp = static_cast<uint8_t*>(sm.mapMemory(0, VK_WHOLE_SIZE));
            const auto* srcp = static_cast<const uint8_t*>(t.rgba);
            for (int row = 0; row < ch; ++row) { // pack the clipped rows tightly
                std::memcpy(dstp + static_cast<size_t>(row) * cw * 4,
                            srcp + (static_cast<size_t>(sy + row) * t.w + sx) * 4,
                            static_cast<size_t>(cw) * 4);
            }
            sm.unmapMemory();

            vk::BufferImageCopy copy{0,
                                     0,
                                     0,
                                     {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                     {dx0, dy0, 0},
                                     {static_cast<uint32_t>(cw), static_cast<uint32_t>(ch), 1}};
            cmd.copyBufferToImage(*sb, *image, vk::ImageLayout::eTransferDstOptimal, copy);
            staging.push_back(std::move(sb));
            staging_memory.push_back(std::move(sm));
        }

        // Transition the image to TransferSrc for the read-back copy.
        vk::ImageMemoryBarrier to_src{vk::AccessFlagBits::eTransferWrite,
                                      vk::AccessFlagBits::eTransferRead,
                                      vk::ImageLayout::eTransferDstOptimal,
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      *image,
                                      {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_src);

        vk::BufferImageCopy region{0,
                                   0,
                                   0,
                                   {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   {0, 0, 0},
                                   {uw, uh, 1}};
        cmd.copyImageToBuffer(*image, vk::ImageLayout::eTransferSrcOptimal, *readback, region);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                            {}, vk::MemoryBarrier{vk::AccessFlagBits::eTransferWrite,
                                                  vk::AccessFlagBits::eHostRead},
                            {}, {});
        cmd.end();

        vk::raii::Fence fence{device, vk::FenceCreateInfo{}};
        vk::SubmitInfo submit;
        submit.setCommandBuffers(*cmd);
        impl_->queue.submit(submit, *fence);
        if (device.waitForFences(*fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()) !=
            vk::Result::eSuccess) {
            return fail("fence wait failed");
        }

        std::vector<Pixel> out(static_cast<size_t>(uw) * uh);
        auto* base = static_cast<std::uint8_t*>(readback_memory.mapMemory(0, VK_WHOLE_SIZE));
        std::memcpy(out.data(), base, static_cast<size_t>(buffer_size));
        readback_memory.unmapMemory();
        return out;
    } catch (const std::exception& e) {
        return fail(std::string{"vulkan composite: "} + e.what());
    }
}

// =============================================================================
// linux-dmabuf import/export (any DRM modifier)
// =============================================================================
namespace {

// DRM ARGB8888/XRGB8888 are little-endian BGRA bytes == VK_FORMAT_B8G8R8A8_UNORM.
vk::Format drm_to_vk(std::uint32_t fourcc) {
    switch (fourcc) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
        return vk::Format::eB8G8R8A8Unorm;
    default:
        return vk::Format::eUndefined;
    }
}

struct DmabufImage {
    vk::raii::Image image;
    vk::raii::DeviceMemory memory;
};

// Create a VkImage backed by an imported single-plane dmabuf. `fd` is borrowed;
// a dup is handed to Vulkan (which owns it on success). Throws on failure.
DmabufImage make_dmabuf_image(const vk::raii::Device& device, int fd, int w, int h, vk::Format fmt,
                              std::uint32_t offset, std::uint32_t stride, std::uint64_t modifier,
                              vk::ImageUsageFlags usage) {
    const vk::SubresourceLayout plane{offset, 0, stride, 0, 0};
    vk::ImageDrmFormatModifierExplicitCreateInfoEXT mod_info{modifier, 1, &plane};
    vk::ExternalMemoryImageCreateInfo ext_img{vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
    ext_img.pNext = &mod_info;

    vk::ImageCreateInfo ci{{},
                           vk::ImageType::e2D,
                           fmt,
                           vk::Extent3D{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1},
                           1,
                           1,
                           vk::SampleCountFlagBits::e1,
                           vk::ImageTiling::eDrmFormatModifierEXT,
                           usage,
                           vk::SharingMode::eExclusive,
                           {},
                           vk::ImageLayout::eUndefined};
    ci.pNext = &ext_img;
    vk::raii::Image image{device, ci};

    // Which memory types accept this dmabuf (query borrows fd, no consume).
    const auto fdprops =
        device.getMemoryFdPropertiesKHR(vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT, fd);
    const auto req = image.getMemoryRequirements();
    const std::uint32_t type_bits = req.memoryTypeBits & fdprops.memoryTypeBits;
    if (type_bits == 0) {
        throw std::runtime_error("no memory type for dmabuf");
    }
    const std::uint32_t mem_type = static_cast<std::uint32_t>(__builtin_ctz(type_bits));

    const int dupfd = dup(fd);
    if (dupfd < 0) {
        throw std::runtime_error("dup(dmabuf fd) failed");
    }
    vk::ImportMemoryFdInfoKHR import_fd{vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT, dupfd};
    vk::MemoryDedicatedAllocateInfo dedicated{*image};
    dedicated.pNext = &import_fd;
    vk::MemoryAllocateInfo alloc{req.size, mem_type};
    alloc.pNext = &dedicated;
    vk::raii::DeviceMemory memory{device, alloc}; // owns dupfd on success
    image.bindMemory(*memory, 0);
    return DmabufImage{std::move(image), std::move(memory)};
}

} // namespace

namespace {

// Single-plane DRM modifiers for `fmt` whose tiling features cover `want`.
std::vector<std::uint64_t> modifiers_with(const vk::raii::PhysicalDevice& physical, vk::Format fmt,
                                          VkFormatFeatureFlags want) {
    // C structs + core-1.1 entrypoint: two-pass (count, then fill).
    VkDrmFormatModifierPropertiesListEXT list{};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    props2.pNext = &list;
    const VkPhysicalDevice phys = static_cast<VkPhysicalDevice>(*physical);
    vkGetPhysicalDeviceFormatProperties2(phys, static_cast<VkFormat>(fmt), &props2);
    std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = mods.data();
    vkGetPhysicalDeviceFormatProperties2(phys, static_cast<VkFormat>(fmt), &props2);

    std::vector<std::uint64_t> out;
    for (const auto& m : mods) {
        if (m.drmFormatModifierPlaneCount == 1 &&
            (m.drmFormatModifierTilingFeatures & want) == want) {
            out.push_back(m.drmFormatModifier);
        }
    }
    return out;
}

} // namespace

bool VulkanRenderer::dmabuf_supported() const noexcept { return impl_->dmabuf_ok; }

std::vector<std::uint64_t> VulkanRenderer::dmabuf_modifiers(std::uint32_t drm_format) {
    if (!impl_->dmabuf_ok) {
        return {};
    }
    const vk::Format fmt = drm_to_vk(drm_format);
    if (fmt == vk::Format::eUndefined) {
        return {};
    }
    // Transfer both ways for the CPU read-back path, sampled so the same buffer
    // can be textured straight onto a scanout target on the GPU path.
    return modifiers_with(impl_->physical, fmt,
                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
}

Result<std::vector<std::uint8_t>> VulkanRenderer::import_dmabuf(int fd, int width, int height,
                                                               std::uint32_t drm_format,
                                                               std::uint32_t offset,
                                                               std::uint32_t stride,
                                                               std::uint64_t modifier) {
    if (!impl_->dmabuf_ok) {
        return fail("dmabuf import unsupported by GPU");
    }
    if (width <= 0 || height <= 0) {
        return fail("invalid dmabuf dimensions");
    }
    const vk::Format fmt = drm_to_vk(drm_format);
    if (fmt == vk::Format::eUndefined) {
        return fail("unsupported dmabuf format");
    }
    try {
        auto& device = impl_->device;
        const auto uw = static_cast<std::uint32_t>(width);
        const auto uh = static_cast<std::uint32_t>(height);
        DmabufImage src = make_dmabuf_image(device, fd, width, height, fmt, offset, stride, modifier,
                                            vk::ImageUsageFlagBits::eTransferSrc);

        const vk::DeviceSize buffer_size = static_cast<vk::DeviceSize>(uw) * uh * 4;
        vk::raii::Buffer readback{device, vk::BufferCreateInfo{{}, buffer_size,
                                                               vk::BufferUsageFlagBits::eTransferDst,
                                                               vk::SharingMode::eExclusive}};
        const auto buf_req = readback.getMemoryRequirements();
        vk::raii::DeviceMemory readback_memory{
            device, vk::MemoryAllocateInfo{
                        buf_req.size,
                        find_memory_type(impl_->physical, buf_req.memoryTypeBits,
                                         vk::MemoryPropertyFlagBits::eHostVisible |
                                             vk::MemoryPropertyFlagBits::eHostCoherent)}};
        readback.bindMemory(*readback_memory, 0);

        vk::raii::CommandBuffers command_buffers{
            device, vk::CommandBufferAllocateInfo{*impl_->command_pool,
                                                  vk::CommandBufferLevel::ePrimary, 1}};
        vk::raii::CommandBuffer& cmd = command_buffers.front();
        cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

        // Acquire the imported image from the foreign (external) owner. For DRM
        // modifier images an Undefined→TransferSrc transition keeps the content
        // (the modifier fixes the memory layout).
        const std::uint32_t foreign =
            impl_->queue_foreign ? uint32_t{VK_QUEUE_FAMILY_FOREIGN_EXT} : VK_QUEUE_FAMILY_IGNORED;
        const std::uint32_t self = impl_->queue_foreign ? impl_->queue_family : VK_QUEUE_FAMILY_IGNORED;
        const vk::ImageSubresourceRange whole{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        vk::ImageMemoryBarrier acquire{{},
                                       vk::AccessFlagBits::eTransferRead,
                                       vk::ImageLayout::eUndefined,
                                       vk::ImageLayout::eTransferSrcOptimal,
                                       foreign,
                                       self,
                                       *src.image,
                                       whole};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, acquire);

        vk::BufferImageCopy region{0, 0, 0, {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   {0, 0, 0}, {uw, uh, 1}};
        cmd.copyImageToBuffer(*src.image, vk::ImageLayout::eTransferSrcOptimal, *readback, region);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                            {}, vk::MemoryBarrier{vk::AccessFlagBits::eTransferWrite,
                                                  vk::AccessFlagBits::eHostRead},
                            {}, {});
        cmd.end();

        vk::raii::Fence fence{device, vk::FenceCreateInfo{}};
        vk::SubmitInfo submit;
        submit.setCommandBuffers(*cmd);
        impl_->queue.submit(submit, *fence);
        if (device.waitForFences(*fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()) !=
            vk::Result::eSuccess) {
            return fail("fence wait failed");
        }

        const bool opaque = drm_format == DRM_FORMAT_XRGB8888;
        std::vector<std::uint8_t> out(static_cast<size_t>(uw) * uh * 4);
        auto* base = static_cast<std::uint8_t*>(readback_memory.mapMemory(0, VK_WHOLE_SIZE));
        for (size_t i = 0; i < static_cast<size_t>(uw) * uh; ++i) {
            // readback is BGRA; emit RGBA.
            out[i * 4 + 0] = base[i * 4 + 2];
            out[i * 4 + 1] = base[i * 4 + 1];
            out[i * 4 + 2] = base[i * 4 + 0];
            out[i * 4 + 3] = opaque ? 255 : base[i * 4 + 3];
        }
        readback_memory.unmapMemory();
        return out;
    } catch (const std::exception& e) {
        return fail(std::string{"vulkan dmabuf import: "} + e.what());
    }
}

Status VulkanRenderer::export_dmabuf(int fd, int width, int height, std::uint32_t drm_format,
                                     std::uint32_t offset, std::uint32_t stride,
                                     std::uint64_t modifier,
                                     const std::vector<std::uint8_t>& rgba) {
    if (!impl_->dmabuf_ok) {
        return fail("dmabuf export unsupported by GPU");
    }
    if (width <= 0 || height <= 0 ||
        rgba.size() < static_cast<size_t>(width) * height * 4) {
        return fail("invalid dmabuf export args");
    }
    const vk::Format fmt = drm_to_vk(drm_format);
    if (fmt == vk::Format::eUndefined) {
        return fail("unsupported dmabuf format");
    }
    try {
        auto& device = impl_->device;
        const auto uw = static_cast<std::uint32_t>(width);
        const auto uh = static_cast<std::uint32_t>(height);
        DmabufImage dst = make_dmabuf_image(device, fd, width, height, fmt, offset, stride, modifier,
                                            vk::ImageUsageFlagBits::eTransferDst);

        // Staging buffer holds BGRA (from RGBA input).
        const vk::DeviceSize buffer_size = static_cast<vk::DeviceSize>(uw) * uh * 4;
        vk::raii::Buffer staging{device, vk::BufferCreateInfo{{}, buffer_size,
                                                              vk::BufferUsageFlagBits::eTransferSrc,
                                                              vk::SharingMode::eExclusive}};
        const auto sreq = staging.getMemoryRequirements();
        vk::raii::DeviceMemory staging_memory{
            device, vk::MemoryAllocateInfo{
                        sreq.size,
                        find_memory_type(impl_->physical, sreq.memoryTypeBits,
                                         vk::MemoryPropertyFlagBits::eHostVisible |
                                             vk::MemoryPropertyFlagBits::eHostCoherent)}};
        staging.bindMemory(*staging_memory, 0);
        auto* sp = static_cast<std::uint8_t*>(staging_memory.mapMemory(0, VK_WHOLE_SIZE));
        for (size_t i = 0; i < static_cast<size_t>(uw) * uh; ++i) {
            sp[i * 4 + 0] = rgba[i * 4 + 2]; // B
            sp[i * 4 + 1] = rgba[i * 4 + 1]; // G
            sp[i * 4 + 2] = rgba[i * 4 + 0]; // R
            sp[i * 4 + 3] = rgba[i * 4 + 3]; // A
        }
        staging_memory.unmapMemory();

        vk::raii::CommandBuffers command_buffers{
            device, vk::CommandBufferAllocateInfo{*impl_->command_pool,
                                                  vk::CommandBufferLevel::ePrimary, 1}};
        vk::raii::CommandBuffer& cmd = command_buffers.front();
        cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

        const std::uint32_t foreign =
            impl_->queue_foreign ? uint32_t{VK_QUEUE_FAMILY_FOREIGN_EXT} : VK_QUEUE_FAMILY_IGNORED;
        const std::uint32_t self = impl_->queue_foreign ? impl_->queue_family : VK_QUEUE_FAMILY_IGNORED;
        const vk::ImageSubresourceRange whole{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        vk::ImageMemoryBarrier acquire{{},
                                       vk::AccessFlagBits::eTransferWrite,
                                       vk::ImageLayout::eUndefined,
                                       vk::ImageLayout::eTransferDstOptimal,
                                       foreign,
                                       self,
                                       *dst.image,
                                       whole};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, acquire);

        vk::BufferImageCopy region{0, 0, 0, {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   {0, 0, 0}, {uw, uh, 1}};
        cmd.copyBufferToImage(*staging, *dst.image, vk::ImageLayout::eTransferDstOptimal, region);

        // Release back to the foreign owner so the client sees the pixels.
        vk::ImageMemoryBarrier release{vk::AccessFlagBits::eTransferWrite,
                                       {},
                                       vk::ImageLayout::eTransferDstOptimal,
                                       vk::ImageLayout::eGeneral,
                                       self,
                                       foreign,
                                       *dst.image,
                                       whole};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, release);
        cmd.end();

        vk::raii::Fence fence{device, vk::FenceCreateInfo{}};
        vk::SubmitInfo submit;
        submit.setCommandBuffers(*cmd);
        impl_->queue.submit(submit, *fence);
        if (device.waitForFences(*fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()) !=
            vk::Result::eSuccess) {
            return fail("fence wait failed");
        }
        return ok();
    } catch (const std::exception& e) {
        return fail(std::string{"vulkan dmabuf export: "} + e.what());
    }
}

// =============================================================================
// GPU compositing: client dmabuf -> texture -> scanout dmabuf -> KMS.
//
// Nothing here reads pixels back to system memory. Client buffers become VkImages
// the GPU samples in place; the frame is composited into an image that is itself
// a dmabuf, which the DRM backend turns into a KMS framebuffer and page-flips.
// =============================================================================

struct GpuTexture::Impl {
    vk::raii::Image image;
    vk::raii::DeviceMemory memory;
    vk::raii::ImageView view;
    int width;
    int height;
    bool external; // imported dmabuf: acquire from the foreign queue on each use

    // The descriptor set that binds this texture, written the first time it is
    // drawn and re-bound from then on. It describes `view`, which never changes
    // for a given Impl, so there is nothing to invalidate. Mutable because a
    // GpuTextureFill borrows the texture by const pointer and this is a cache.
    // Owned by the renderer's pools, hence a raw handle and the back-pointer
    // that returns it when the texture dies.
    mutable vk::DescriptorSet set{};
    mutable VulkanRenderer::Impl* owner = nullptr;
};

struct ScanoutTarget::Impl {
    vk::raii::Image image;
    vk::raii::DeviceMemory memory;
    vk::raii::ImageView view;
    vk::Format format;
    DmabufPlane plane;        // plane.fd is owned here
    bool has_content = false; // false until the first full render
    UniqueFd acquire_fence;   // sync_file from the flip still scanning us out

    // Staging buffer for read_scanout(), created on first use and kept. It is
    // mapped once and stays mapped: re-creating and re-mapping it per frame was
    // most of the cost of reading a frame back.
    std::optional<vk::raii::Buffer> readback;
    std::optional<vk::raii::DeviceMemory> readback_memory;
    std::uint8_t* readback_map = nullptr;
    bool readback_coherent = true; // false: needs an explicit invalidate

    ~Impl() {
        if (plane.fd >= 0) {
            close(plane.fd);
        }
    }
};

GpuTexture::GpuTexture(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuTexture::~GpuTexture() {
    if (impl_ && impl_->owner != nullptr) {
        impl_->owner->retire_set(impl_->set);
    }
}
GpuTexture::GpuTexture(GpuTexture&&) noexcept = default;
GpuTexture& GpuTexture::operator=(GpuTexture&&) noexcept = default;
int GpuTexture::width() const noexcept { return impl_->width; }
int GpuTexture::height() const noexcept { return impl_->height; }

ScanoutTarget::ScanoutTarget(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ScanoutTarget::~ScanoutTarget() = default;
ScanoutTarget::ScanoutTarget(ScanoutTarget&&) noexcept = default;
ScanoutTarget& ScanoutTarget::operator=(ScanoutTarget&&) noexcept = default;
const DmabufPlane& ScanoutTarget::plane() const noexcept { return impl_->plane; }

void ScanoutTarget::set_acquire_fence(int fd) noexcept {
    impl_->acquire_fence.reset(fd);
}

Status VulkanRenderer::read_scanout(ScanoutTarget& target, std::vector<std::uint8_t>& out) {
    try {
        auto& device = impl_->device;
        ScanoutTarget::Impl& t = *target.impl_;
        const auto uw = static_cast<std::uint32_t>(t.plane.width);
        const auto uh = static_cast<std::uint32_t>(t.plane.height);
        const vk::DeviceSize size = static_cast<vk::DeviceSize>(uw) * uh * 4;

        if (!t.readback.has_value()) {
            t.readback.emplace(device,
                               vk::BufferCreateInfo{{},
                                                    size,
                                                    vk::BufferUsageFlagBits::eTransferDst,
                                                    vk::SharingMode::eExclusive});
            const auto req = t.readback->getMemoryRequirements();
            // HOST_CACHED first: the conversion below reads every byte back, and
            // scalar reads out of uncached (write-combined) memory run at a few
            // tens of MB/s — which is where the 90ms went.
            std::uint32_t type = 0;
            try {
                type = find_memory_type(impl_->physical, req.memoryTypeBits,
                                        vk::MemoryPropertyFlagBits::eHostVisible |
                                            vk::MemoryPropertyFlagBits::eHostCached);
                t.readback_coherent = false;
            } catch (const std::exception&) {
                type = find_memory_type(impl_->physical, req.memoryTypeBits,
                                        vk::MemoryPropertyFlagBits::eHostVisible |
                                            vk::MemoryPropertyFlagBits::eHostCoherent);
                t.readback_coherent = true;
            }
            t.readback_memory.emplace(device, vk::MemoryAllocateInfo{req.size, type});
            t.readback->bindMemory(**t.readback_memory, 0);
            t.readback_map =
                static_cast<std::uint8_t*>(t.readback_memory->mapMemory(0, VK_WHOLE_SIZE));
        }

        vk::raii::CommandBuffers command_buffers{
            device, vk::CommandBufferAllocateInfo{*impl_->command_pool,
                                                  vk::CommandBufferLevel::ePrimary, 1}};
        vk::raii::CommandBuffer& cmd = command_buffers.front();
        cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

        const vk::ImageSubresourceRange whole{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        // render_to leaves the image in eGeneral, released to the foreign owner.
        vk::ImageMemoryBarrier to_src{{},
                                      vk::AccessFlagBits::eTransferRead,
                                      vk::ImageLayout::eGeneral,
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      *t.image,
                                      whole};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_src);
        vk::BufferImageCopy region{0, 0, 0, {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   {0, 0, 0}, {uw, uh, 1}};
        cmd.copyImageToBuffer(*t.image, vk::ImageLayout::eTransferSrcOptimal, **t.readback, region);
        // Put it back where render_to and the display engine expect to find it.
        vk::ImageMemoryBarrier back{vk::AccessFlagBits::eTransferRead,
                                    {},
                                    vk::ImageLayout::eTransferSrcOptimal,
                                    vk::ImageLayout::eGeneral,
                                    VK_QUEUE_FAMILY_IGNORED,
                                    VK_QUEUE_FAMILY_IGNORED,
                                    *t.image,
                                    whole};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, back);
        cmd.end();

        vk::raii::Fence fence{device, vk::FenceCreateInfo{}};
        vk::SubmitInfo submit;
        submit.setCommandBuffers(*cmd);
        impl_->queue.submit(submit, *fence);
        if (device.waitForFences(*fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()) !=
            vk::Result::eSuccess) {
            return fail("fence wait failed");
        }
        if (!t.readback_coherent) {
            device.invalidateMappedMemoryRanges(
                vk::MappedMemoryRange{**t.readback_memory, 0, VK_WHOLE_SIZE});
        }

        const bool opaque = t.plane.format == DRM_FORMAT_XRGB8888;
        out.resize(static_cast<size_t>(size));
        const std::uint8_t* base = t.readback_map;
        for (size_t i = 0; i < static_cast<size_t>(uw) * uh; ++i) {
            out[i * 4 + 0] = base[i * 4 + 2]; // BGRA in, RGBA out
            out[i * 4 + 1] = base[i * 4 + 1];
            out[i * 4 + 2] = base[i * 4 + 0];
            out[i * 4 + 3] = opaque ? 255 : base[i * 4 + 3];
        }
        return ok();
    } catch (const std::exception& e) {
        return fail(std::string{"vulkan read_scanout: "} + e.what());
    }
}

namespace {

/// A view for sampling. Opaque formats (XRGB) carry garbage in the alpha byte,
/// so swizzle alpha to 1 rather than branching in the shader.
vk::raii::ImageView make_sampler_view(const vk::raii::Device& device, const vk::raii::Image& image,
                                      vk::Format format, bool opaque) {
    const vk::ComponentMapping swizzle{vk::ComponentSwizzle::eIdentity,
                                       vk::ComponentSwizzle::eIdentity,
                                       vk::ComponentSwizzle::eIdentity,
                                       opaque ? vk::ComponentSwizzle::eOne
                                              : vk::ComponentSwizzle::eIdentity};
    return vk::raii::ImageView{
        device, vk::ImageViewCreateInfo{{},
                                        *image,
                                        vk::ImageViewType::e2D,
                                        format,
                                        swizzle,
                                        {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}};
}

/// Which source corner each destination corner samples, for the four device
/// corners in triangle-strip order (TL, TR, BL, BR). This is where output
/// rotation actually happens: the quad is already placed in device space, and
/// the UVs turn the picture inside it.
std::array<std::array<float, 2>, 4> corner_uvs(Transform t, float u0, float v0, float u1,
                                               float v1) {
    if (transform_flipped(t)) {
        std::swap(u0, u1); // mirrored along the logical x axis, before rotating
    }
    switch (transform_rotation(t)) {
    case 90:
        return {{{u0, v1}, {u0, v0}, {u1, v1}, {u1, v0}}};
    case 180:
        return {{{u1, v1}, {u0, v1}, {u1, v0}, {u0, v0}}};
    case 270:
        return {{{u1, v0}, {u1, v1}, {u0, v0}, {u0, v1}}};
    default:
        return {{{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}}};
    }
}

struct QuadPush {
    float rect[4];
    float uv01[4];
    float uv23[4];
};

vk::raii::RenderPass make_pass(const vk::raii::Device& device, vk::Format format, bool load) {
    vk::AttachmentDescription attachment{
        {},
        format,
        vk::SampleCountFlagBits::e1,
        load ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear,
        vk::AttachmentStoreOp::eStore,
        vk::AttachmentLoadOp::eDontCare,
        vk::AttachmentStoreOp::eDontCare,
        load ? vk::ImageLayout::eColorAttachmentOptimal : vk::ImageLayout::eUndefined,
        vk::ImageLayout::eColorAttachmentOptimal};
    vk::AttachmentReference color_ref{0, vk::ImageLayout::eColorAttachmentOptimal};
    vk::SubpassDescription subpass{{}, vk::PipelineBindPoint::eGraphics, {}, color_ref};
    return vk::raii::RenderPass{device, vk::RenderPassCreateInfo{{}, attachment, subpass}};
}

} // namespace

// One binding, one sampler, the same for every target format — so there is one
// layout for the whole renderer and every cached set is interchangeable.
vk::DescriptorSetLayout VulkanRenderer::Impl::quad_set_layout() {
    if (!set_layout.has_value()) {
        vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eCombinedImageSampler, 1,
                                               vk::ShaderStageFlagBits::eFragment};
        set_layout.emplace(device, vk::DescriptorSetLayoutCreateInfo{{}, binding});
    }
    return **set_layout;
}

/// A set for one texture, reused from a dead texture's if there is one. Pools
/// are grown, never freed: a set costs a handful of bytes and the count is
/// bounded by how many client buffers are on screen at once.
vk::DescriptorSet VulkanRenderer::Impl::acquire_set() {
    if (!free_sets.empty()) {
        const vk::DescriptorSet set = free_sets.back();
        free_sets.pop_back();
        return set;
    }
    constexpr std::uint32_t kPerPool = 64;
    if (set_pools.empty() || sets_in_pool == kPerPool) {
        vk::DescriptorPoolSize size{vk::DescriptorType::eCombinedImageSampler, kPerPool};
        set_pools.emplace_back(device, vk::DescriptorPoolCreateInfo{{}, kPerPool, size});
        sets_in_pool = 0;
    }
    vk::DescriptorSetLayout raw_layout = quad_set_layout();
    vk::raii::DescriptorSets sets{
        device, vk::DescriptorSetAllocateInfo{*set_pools.back(), raw_layout}};
    ++sets_in_pool;
    // The pool owns the set for the renderer's lifetime; the raii wrapper must
    // not try to give it back.
    const vk::DescriptorSet raw = *sets.front();
    sets.front().release();
    return raw;
}

QuadPipeline& VulkanRenderer::Impl::quad_pipeline(vk::Format format) {
    for (QuadPipeline& p : pipelines) {
        if (p.format == format) {
            return p;
        }
    }

    vk::PushConstantRange push{vk::ShaderStageFlagBits::eVertex, 0, sizeof(QuadPush)};
    vk::DescriptorSetLayout raw_set_layout = quad_set_layout();
    vk::raii::PipelineLayout layout{
        device, vk::PipelineLayoutCreateInfo{{}, raw_set_layout, push}};

    vk::raii::ShaderModule vert{
        device, vk::ShaderModuleCreateInfo{{}, sizeof(kQuadVertSpv), kQuadVertSpv}};
    vk::raii::ShaderModule frag{
        device, vk::ShaderModuleCreateInfo{{}, sizeof(kQuadFragSpv), kQuadFragSpv}};
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages{
        vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eVertex, *vert, "main"},
        vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eFragment, *frag, "main"}};

    vk::PipelineVertexInputStateCreateInfo vertex_input{}; // positions come from push constants
    vk::PipelineInputAssemblyStateCreateInfo assembly{{}, vk::PrimitiveTopology::eTriangleStrip};
    vk::PipelineViewportStateCreateInfo viewport{{}, 1, nullptr, 1, nullptr}; // dynamic
    vk::PipelineRasterizationStateCreateInfo raster{};
    raster.lineWidth = 1.0f;
    raster.cullMode = vk::CullModeFlagBits::eNone;
    vk::PipelineMultisampleStateCreateInfo multisample{};
    // Wayland buffers are pre-multiplied, so ONE / ONE_MINUS_SRC_ALPHA is the
    // correct over-operator — translucent surfaces now blend instead of stamping.
    vk::PipelineColorBlendAttachmentState blend{
        VK_TRUE,
        vk::BlendFactor::eOne,
        vk::BlendFactor::eOneMinusSrcAlpha,
        vk::BlendOp::eAdd,
        vk::BlendFactor::eOne,
        vk::BlendFactor::eOneMinusSrcAlpha,
        vk::BlendOp::eAdd,
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
    vk::PipelineColorBlendStateCreateInfo blending{{}, VK_FALSE, vk::LogicOp::eCopy, blend};
    const std::array<vk::DynamicState, 2> dynamic_states{vk::DynamicState::eViewport,
                                                         vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic{{}, dynamic_states};

    vk::raii::RenderPass load_pass = make_pass(device, format, true);
    vk::raii::RenderPass clear_pass = make_pass(device, format, false);

    vk::GraphicsPipelineCreateInfo info{{}, stages, &vertex_input, &assembly, nullptr, &viewport,
                                        &raster, &multisample, nullptr, &blending, &dynamic,
                                        *layout, *clear_pass, 0};
    vk::raii::Pipeline pipeline{device, nullptr, info};

    pipelines.push_back(QuadPipeline{format, std::move(layout), std::move(load_pass),
                                     std::move(clear_pass), std::move(pipeline)});
    return pipelines.back();
}

Result<GpuTexture> VulkanRenderer::import_texture(const DmabufPlane& p) {
    if (!impl_->dmabuf_ok) {
        return fail("dmabuf import unsupported by GPU");
    }
    if (p.width <= 0 || p.height <= 0) {
        return fail("invalid dmabuf dimensions");
    }
    const vk::Format fmt = drm_to_vk(p.format);
    if (fmt == vk::Format::eUndefined) {
        return fail("unsupported dmabuf format");
    }
    try {
        DmabufImage img = make_dmabuf_image(impl_->device, p.fd, p.width, p.height, fmt, p.offset,
                                            p.stride, p.modifier, vk::ImageUsageFlagBits::eSampled);
        // XRGB has no meaningful alpha byte; force it to opaque in the view.
        vk::raii::ImageView view = make_sampler_view(impl_->device, img.image, fmt,
                                                     p.format == DRM_FORMAT_XRGB8888);
        return GpuTexture{std::make_unique<GpuTexture::Impl>(
            GpuTexture::Impl{std::move(img.image), std::move(img.memory), std::move(view), p.width,
                             p.height, true})};
    } catch (const std::exception& e) {
        return fail(std::string{"vulkan texture import: "} + e.what());
    }
}

Result<GpuTexture> VulkanRenderer::upload_texture(int width, int height,
                                                  std::span<const std::uint8_t> rgba) {
    if (width <= 0 || height <= 0) {
        return fail("invalid texture dimensions");
    }
    if (rgba.size() < static_cast<size_t>(width) * height * 4) {
        return fail("texture pixel data too small");
    }
    try {
        auto& device = impl_->device;
        const auto uw = static_cast<std::uint32_t>(width);
        const auto uh = static_cast<std::uint32_t>(height);

        vk::raii::Image image{device,
                              vk::ImageCreateInfo{{},
                                                  vk::ImageType::e2D,
                                                  vk::Format::eR8G8B8A8Unorm,
                                                  vk::Extent3D{uw, uh, 1},
                                                  1,
                                                  1,
                                                  vk::SampleCountFlagBits::e1,
                                                  vk::ImageTiling::eOptimal,
                                                  vk::ImageUsageFlagBits::eTransferDst |
                                                      vk::ImageUsageFlagBits::eSampled,
                                                  vk::SharingMode::eExclusive,
                                                  {},
                                                  vk::ImageLayout::eUndefined}};
        const auto req = image.getMemoryRequirements();
        vk::raii::DeviceMemory memory{
            device, vk::MemoryAllocateInfo{req.size,
                                           find_memory_type(impl_->physical, req.memoryTypeBits,
                                                            vk::MemoryPropertyFlagBits::eDeviceLocal)}};
        image.bindMemory(*memory, 0);

        const vk::DeviceSize size = static_cast<vk::DeviceSize>(uw) * uh * 4;
        vk::raii::Buffer staging{device, vk::BufferCreateInfo{{}, size,
                                                              vk::BufferUsageFlagBits::eTransferSrc,
                                                              vk::SharingMode::eExclusive}};
        const auto sreq = staging.getMemoryRequirements();
        vk::raii::DeviceMemory staging_memory{
            device, vk::MemoryAllocateInfo{
                        sreq.size,
                        find_memory_type(impl_->physical, sreq.memoryTypeBits,
                                         vk::MemoryPropertyFlagBits::eHostVisible |
                                             vk::MemoryPropertyFlagBits::eHostCoherent)}};
        staging.bindMemory(*staging_memory, 0);
        auto* sp = static_cast<std::uint8_t*>(staging_memory.mapMemory(0, VK_WHOLE_SIZE));
        std::memcpy(sp, rgba.data(), static_cast<size_t>(size));
        staging_memory.unmapMemory();

        vk::raii::CommandBuffers command_buffers{
            device, vk::CommandBufferAllocateInfo{*impl_->command_pool,
                                                  vk::CommandBufferLevel::ePrimary, 1}};
        vk::raii::CommandBuffer& cmd = command_buffers.front();
        cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

        const vk::ImageSubresourceRange whole{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        vk::ImageMemoryBarrier to_dst{{},
                                      vk::AccessFlagBits::eTransferWrite,
                                      vk::ImageLayout::eUndefined,
                                      vk::ImageLayout::eTransferDstOptimal,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      *image,
                                      whole};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_dst);
        vk::BufferImageCopy region{
            0, 0, 0, {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, {0, 0, 0}, {uw, uh, 1}};
        cmd.copyBufferToImage(*staging, *image, vk::ImageLayout::eTransferDstOptimal, region);
        // Park it in ShaderReadOnlyOptimal: that is where render_to() expects it,
        // so repeated frames re-use the texture with no further transition.
        vk::ImageMemoryBarrier to_read{vk::AccessFlagBits::eTransferWrite,
                                       vk::AccessFlagBits::eShaderRead,
                                       vk::ImageLayout::eTransferDstOptimal,
                                       vk::ImageLayout::eShaderReadOnlyOptimal,
                                       VK_QUEUE_FAMILY_IGNORED,
                                       VK_QUEUE_FAMILY_IGNORED,
                                       *image,
                                       whole};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, to_read);
        cmd.end();

        vk::raii::Fence fence{device, vk::FenceCreateInfo{}};
        vk::SubmitInfo submit;
        submit.setCommandBuffers(*cmd);
        impl_->queue.submit(submit, *fence);
        if (device.waitForFences(*fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()) !=
            vk::Result::eSuccess) {
            return fail("fence wait failed");
        }
        // current_buffer_rgba() already forces alpha to 255 for opaque formats,
        // so this view needs no swizzle.
        vk::raii::ImageView view =
            make_sampler_view(impl_->device, image, vk::Format::eR8G8B8A8Unorm, false);
        return GpuTexture{std::make_unique<GpuTexture::Impl>(GpuTexture::Impl{
            std::move(image), std::move(memory), std::move(view), width, height, false})};
    } catch (const std::exception& e) {
        return fail(std::string{"vulkan texture upload: "} + e.what());
    }
}

std::vector<std::uint64_t> VulkanRenderer::scanout_modifiers(std::uint32_t drm_format) {
    if (!impl_->dmabuf_ok) {
        return {};
    }
    const vk::Format fmt = drm_to_vk(drm_format);
    if (fmt == vk::Format::eUndefined) {
        return {};
    }
    // We render into it (colour attachment, with blending) and hand the pixels
    // out for screencopy, so both features must be there.
    return modifiers_with(impl_->physical, fmt,
                          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                              VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                              VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
}

Result<ScanoutTarget> VulkanRenderer::create_scanout(int width, int height,
                                                     std::uint32_t drm_format,
                                                     std::span<const std::uint64_t> modifiers) {
    if (!impl_->dmabuf_ok) {
        return fail("dmabuf export unsupported by GPU");
    }
    if (width <= 0 || height <= 0) {
        return fail("invalid scanout dimensions");
    }
    const vk::Format fmt = drm_to_vk(drm_format);
    if (fmt == vk::Format::eUndefined) {
        return fail("unsupported scanout format");
    }
    // Empty request means "whatever works" — LINEAR always does.
    std::vector<std::uint64_t> candidates(modifiers.begin(), modifiers.end());
    if (candidates.empty()) {
        candidates.push_back(DRM_FORMAT_MOD_LINEAR);
    }
    try {
        auto& device = impl_->device;
        const auto uw = static_cast<std::uint32_t>(width);
        const auto uh = static_cast<std::uint32_t>(height);

        vk::ImageDrmFormatModifierListCreateInfoEXT mod_list{candidates};
        vk::ExternalMemoryImageCreateInfo ext_img{vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
        ext_img.pNext = &mod_list;
        vk::ImageCreateInfo ci{{},
                               vk::ImageType::e2D,
                               fmt,
                               vk::Extent3D{uw, uh, 1},
                               1,
                               1,
                               vk::SampleCountFlagBits::e1,
                               vk::ImageTiling::eDrmFormatModifierEXT,
                               vk::ImageUsageFlagBits::eColorAttachment |
                                   vk::ImageUsageFlagBits::eTransferDst |
                                   vk::ImageUsageFlagBits::eTransferSrc,
                               vk::SharingMode::eExclusive,
                               {},
                               vk::ImageLayout::eUndefined};
        ci.pNext = &ext_img;
        vk::raii::Image image{device, ci};

        const auto req = image.getMemoryRequirements();
        vk::ExportMemoryAllocateInfo export_info{
            vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
        vk::MemoryDedicatedAllocateInfo dedicated{*image};
        dedicated.pNext = &export_info;
        vk::MemoryAllocateInfo alloc{req.size,
                                     find_memory_type(impl_->physical, req.memoryTypeBits,
                                                      vk::MemoryPropertyFlagBits::eDeviceLocal)};
        alloc.pNext = &dedicated;
        vk::raii::DeviceMemory memory{device, alloc};
        image.bindMemory(*memory, 0);

        vk::raii::ImageView view{
            device, vk::ImageViewCreateInfo{{},
                                            *image,
                                            vk::ImageViewType::e2D,
                                            fmt,
                                            {},
                                            {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}};

        const int fd = device.getMemoryFdKHR(
            vk::MemoryGetFdInfoKHR{*memory, vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT});
        const auto chosen = image.getDrmFormatModifierPropertiesEXT();
        const auto layout = image.getSubresourceLayout(
            vk::ImageSubresource{vk::ImageAspectFlagBits::eMemoryPlane0EXT, 0, 0});

        DmabufPlane plane{fd,
                          width,
                          height,
                          drm_format,
                          static_cast<std::uint32_t>(layout.offset),
                          static_cast<std::uint32_t>(layout.rowPitch),
                          chosen.drmFormatModifier};
        // Aggregate-new, not make_unique: Impl closes the fd in its destructor,
        // which costs it the implicit move constructor.
        return ScanoutTarget{std::unique_ptr<ScanoutTarget::Impl>(
            new ScanoutTarget::Impl{std::move(image), std::move(memory), std::move(view), fmt,
                                    plane, false, UniqueFd{}, std::nullopt, std::nullopt, nullptr,
                                    true})};
    } catch (const std::exception& e) {
        return fail(std::string{"vulkan scanout alloc: "} + e.what());
    }
}

Status VulkanRenderer::render_to(ScanoutTarget& target, Color background,
                                 std::span<const RectFill> rects,
                                 std::span<const GpuTextureFill> textures,
                                 std::span<const Box> damage, const OutputMapping& mapping,
                                 const RenderSync& sync) {
    try {
        auto& device = impl_->device;
        impl_->reap();
        ScanoutTarget::Impl& t = *target.impl_;
        const int device_w = t.plane.width;
        const int device_h = t.plane.height;
        const int scale = mapping.scale < 1 ? 1 : mapping.scale;
        const Transform transform = mapping.transform;
        // Everything the caller hands us is in logical coordinates; the target is
        // device pixels. This is the only place the two meet.
        const int logical_w = (transform_swaps_axes(transform) ? device_h : device_w) / scale;
        const int logical_h = (transform_swaps_axes(transform) ? device_w : device_h) / scale;
        auto to_device = [&](const Box& b) {
            return transform_box(transform, scale, b, device_w, device_h);
        };
        auto to_rect2d = [](const Box& b) {
            return vk::Rect2D{{b.x, b.y},
                              {static_cast<uint32_t>(b.width), static_cast<uint32_t>(b.height)}};
        };

        // What to touch. The damage boxes become a disjoint region: overlapping
        // rects would blend a translucent surface into the same pixel twice, and
        // the render pass needs one area that covers them all.
        const Box full{0, 0, logical_w, logical_h};
        Region repaint;
        const bool partial = !damage.empty() && t.has_content;
        if (partial) {
            for (const Box& d : damage) {
                repaint.add(d.intersection(full));
            }
            if (repaint.empty()) {
                return ok(); // nothing changed; the target already shows it
            }
        } else {
            repaint.add(full);
        }

        // Occlusion: walk front to back and remember what is already covered by
        // something the caller declared opaque. A maximised window over a
        // wallpaper means the wallpaper is never sampled at all.
        std::vector<Region> visible(textures.size());
        Region covered;
        for (size_t i = textures.size(); i-- > 0;) {
            const GpuTextureFill& tf = textures[i];
            if (tf.texture == nullptr || tf.w <= 0 || tf.h <= 0) {
                continue;
            }
            visible[i] = repaint;
            visible[i].intersect(Box{tf.x, tf.y, tf.w, tf.h});
            visible[i].subtract(covered);
            // Clipped to the destination rect box by box: a Region copy here
            // would be one heap allocation per texture per frame.
            for (const Box& b : tf.opaque) {
                if (const Box hit = b.intersection(Box{tf.x, tf.y, tf.w, tf.h}); !hit.empty()) {
                    covered.add(hit);
                }
            }
        }
        // Whatever is left over is background and solid rects, the very back.
        Region backdrop = repaint;
        backdrop.subtract(covered);

        const Box device_area = to_device(repaint.extents());

        QuadPipeline& quad = impl_->quad_pipeline(t.format);
        vk::ImageView fb_attachment = *t.view;
        vk::raii::Framebuffer framebuffer{
            device, vk::FramebufferCreateInfo{{},
                                              partial ? *quad.load_pass : *quad.clear_pass,
                                              fb_attachment,
                                              static_cast<std::uint32_t>(device_w),
                                              static_cast<std::uint32_t>(device_h),
                                              1}};

        vk::raii::CommandBuffers command_buffers{
            device, vk::CommandBufferAllocateInfo{*impl_->command_pool,
                                                  vk::CommandBufferLevel::ePrimary, 1}};
        vk::raii::CommandBuffer& cmd = command_buffers.front();
        cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

        const std::uint32_t foreign =
            impl_->queue_foreign ? uint32_t{VK_QUEUE_FAMILY_FOREIGN_EXT} : VK_QUEUE_FAMILY_IGNORED;
        const std::uint32_t self =
            impl_->queue_foreign ? impl_->queue_family : VK_QUEUE_FAMILY_IGNORED;
        const vk::ImageSubresourceRange whole{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

        // Take the target back from the display engine. On a partial repaint the
        // untouched pixels must survive, so we come in from the layout it left
        // rather than from Undefined.
        vk::ImageMemoryBarrier reacquire{{},
                                         vk::AccessFlagBits::eColorAttachmentWrite,
                                         partial ? vk::ImageLayout::eGeneral
                                                 : vk::ImageLayout::eUndefined,
                                         vk::ImageLayout::eColorAttachmentOptimal,
                                         partial ? foreign : VK_QUEUE_FAMILY_IGNORED,
                                         partial ? self : VK_QUEUE_FAMILY_IGNORED,
                                         *t.image,
                                         whole};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {},
                            reacquire);

        // Take every source texture for sampling. Imported client buffers come
        // from the foreign (external) queue; ours already sit in the read layout.
        for (const GpuTextureFill& tf : textures) {
            if (tf.texture == nullptr) {
                continue;
            }
            const GpuTexture::Impl& tex = *tf.texture->impl_;
            vk::ImageMemoryBarrier acquire{
                {},
                vk::AccessFlagBits::eShaderRead,
                tex.external ? vk::ImageLayout::eUndefined
                             : vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal,
                tex.external ? foreign : VK_QUEUE_FAMILY_IGNORED,
                tex.external ? self : VK_QUEUE_FAMILY_IGNORED,
                *tex.image,
                whole};
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, acquire);
        }

        vk::ClearValue clear{vk::ClearColorValue{
            std::array<float, 4>{background.r, background.g, background.b, background.a}}};
        const vk::Rect2D render_area = to_rect2d(device_area);
        cmd.beginRenderPass(vk::RenderPassBeginInfo{partial ? *quad.load_pass : *quad.clear_pass,
                                                    *framebuffer, render_area, clear},
                            vk::SubpassContents::eInline);

        // On a partial repaint loadOp is eLoad, so the background has to be
        // painted explicitly — over each damage box, not over the box spanning
        // them, and not at all where an opaque surface will cover it anyway.
        if (partial) {
            std::vector<vk::ClearRect> clear_rects;
            clear_rects.reserve(backdrop.rects().size());
            for (const Box& b : backdrop.rects()) {
                clear_rects.push_back(vk::ClearRect{to_rect2d(to_device(b)), 0, 1});
            }
            if (!clear_rects.empty()) {
                vk::ClearAttachment clear_att{
                    vk::ImageAspectFlagBits::eColor, 0,
                    vk::ClearColorValue{std::array<float, 4>{background.r, background.g,
                                                             background.b, background.a}}};
                cmd.clearAttachments(clear_att, clear_rects);
            }
        }
        for (const RectFill& rf : rects) {
            std::vector<vk::ClearRect> clear_rects;
            for (const Box& b : backdrop.rects()) {
                if (const Box hit = rf.box.intersection(b); !hit.empty()) {
                    clear_rects.push_back(vk::ClearRect{to_rect2d(to_device(hit)), 0, 1});
                }
            }
            if (clear_rects.empty()) {
                continue;
            }
            vk::ClearAttachment clear_att{
                vk::ImageAspectFlagBits::eColor, 0,
                vk::ClearColorValue{
                    std::array<float, 4>{rf.color.r, rf.color.g, rf.color.b, rf.color.a}}};
            cmd.clearAttachments(clear_att, clear_rects);
        }

        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *quad.pipeline);
        cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(device_w),
                                        static_cast<float>(device_h), 0.0f, 1.0f});

        for (size_t i = 0; i < textures.size(); ++i) {
            const GpuTextureFill& tf = textures[i];
            if (visible[i].empty()) {
                continue; // fully occluded, off-screen, or outside the damage
            }
            const GpuTexture::Impl& tex = *tf.texture->impl_;

            // The quad covers the whole destination rect; the scissor does the
            // clipping. The source is sampled through the buffer transform folded
            // into the output's, so one draw handles both rotations.
            const Box dev = to_device(Box{tf.x, tf.y, tf.w, tf.h});
            const auto uv =
                corner_uvs(transform_compose(transform, tf.transform), tf.u0, tf.v0, tf.u1, tf.v1);
            QuadPush push{};
            push.rect[0] = 2.0f * static_cast<float>(dev.x) / static_cast<float>(device_w) - 1.0f;
            push.rect[1] = 2.0f * static_cast<float>(dev.y) / static_cast<float>(device_h) - 1.0f;
            push.rect[2] =
                2.0f * static_cast<float>(dev.x + dev.width) / static_cast<float>(device_w) - 1.0f;
            push.rect[3] =
                2.0f * static_cast<float>(dev.y + dev.height) / static_cast<float>(device_h) - 1.0f;
            push.uv01[0] = uv[0][0];
            push.uv01[1] = uv[0][1];
            push.uv01[2] = uv[1][0];
            push.uv01[3] = uv[1][1];
            push.uv23[0] = uv[2][0];
            push.uv23[1] = uv[2][1];
            push.uv23[2] = uv[3][0];
            push.uv23[3] = uv[3][1];

            // Written once per texture, then only bound. The same window drawn
            // every frame costs one bindDescriptorSets and nothing else.
            if (!tex.set) {
                tex.set = impl_->acquire_set();
                tex.owner = impl_.get();
                vk::DescriptorImageInfo image_info{**impl_->sampler, *tex.view,
                                                   vk::ImageLayout::eShaderReadOnlyOptimal};
                device.updateDescriptorSets(
                    vk::WriteDescriptorSet{tex.set, 0, 0,
                                           vk::DescriptorType::eCombinedImageSampler, image_info},
                    {});
            }
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *quad.layout, 0, tex.set, {});
            cmd.pushConstants<QuadPush>(*quad.layout, vk::ShaderStageFlagBits::eVertex, 0, push);
            // One draw per visible box: the vertex work is four vertices, and
            // the fragments outside the scissor never happen.
            for (const Box& b : visible[i].rects()) {
                cmd.setScissor(0, to_rect2d(to_device(b)));
                cmd.draw(4, 1, 0, 0);
            }
        }
        cmd.endRenderPass();

        // Release the target back to the foreign owner: the display engine reads
        // it next, not us.
        vk::ImageMemoryBarrier release{vk::AccessFlagBits::eColorAttachmentWrite,
                                       {},
                                       vk::ImageLayout::eColorAttachmentOptimal,
                                       vk::ImageLayout::eGeneral,
                                       self,
                                       foreign,
                                       *t.image,
                                       whole};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                            vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, release);
        cmd.end();

        // --- explicit sync: wait on the GPU, not on the CPU ---
        //
        // Every fence a client gave us for its buffer, plus whatever the display
        // engine still owes on this target, becomes a semaphore the queue waits
        // on. The finished render becomes a sync_file the caller hands to KMS.
        std::vector<vk::raii::Semaphore> semaphores;
        std::vector<vk::Semaphore> wait_handles;
        std::vector<vk::PipelineStageFlags> wait_stages;
        auto add_wait = [&](int fd) {
            if (fd < 0 || !impl_->sync_fd_ok) {
                return;
            }
            const int dupfd = dup(fd); // the import consumes the fd it is given
            if (dupfd < 0) {
                return;
            }
            vk::raii::Semaphore sem{device, vk::SemaphoreCreateInfo{}};
            device.importSemaphoreFdKHR(vk::ImportSemaphoreFdInfoKHR{
                *sem, vk::SemaphoreImportFlagBits::eTemporary,
                vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd, dupfd});
            wait_handles.push_back(*sem);
            wait_stages.push_back(vk::PipelineStageFlagBits::eAllCommands);
            semaphores.push_back(std::move(sem));
        };
        for (int fd : sync.wait_fds) {
            add_wait(fd);
        }
        add_wait(t.acquire_fence.get());

        const bool want_out_fence = sync.out_fence_fd != nullptr && impl_->sync_fd_ok;
        std::optional<vk::raii::Semaphore> out_sem;
        if (want_out_fence) {
            vk::ExportSemaphoreCreateInfo export_info{
                vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd};
            vk::SemaphoreCreateInfo ci{};
            ci.pNext = &export_info;
            out_sem.emplace(device, ci);
        }

        vk::raii::Fence fence{device, vk::FenceCreateInfo{}};
        vk::SubmitInfo submit;
        submit.setCommandBuffers(*cmd);
        if (!wait_handles.empty()) {
            submit.setWaitSemaphores(wait_handles);
            submit.setWaitDstStageMask(wait_stages);
        }
        vk::Semaphore out_handle;
        if (out_sem.has_value()) {
            out_handle = **out_sem;
            submit.setSignalSemaphores(out_handle);
        }
        impl_->queue.submit(submit, *fence);

        // The waits consumed the target's fence; a stale one must not be waited
        // on twice.
        t.acquire_fence.reset();
        t.has_content = true;

        if (out_sem.has_value()) {
            *sync.out_fence_fd = device.getSemaphoreFdKHR(vk::SemaphoreGetFdInfoKHR{
                **out_sem, vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd});
            semaphores.push_back(std::move(*out_sem));
            // Nothing is waited on here: the command buffer, its descriptors and
            // these semaphores stay alive in the in-flight list until the fence
            // says the GPU is done with them.
            impl_->in_flight.push_back(InFlight{std::move(fence), std::move(command_buffers),
                                                std::move(framebuffer), std::move(semaphores),
                                                ++impl_->submits});
            return ok();
        }
        if (sync.out_fence_fd != nullptr) {
            *sync.out_fence_fd = -1; // asked for a fence the device cannot give
        }
        if (device.waitForFences(*fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()) !=
            vk::Result::eSuccess) {
            return fail("fence wait failed");
        }
        return ok();
    } catch (const std::exception& e) {
        return fail(std::string{"vulkan render_to: "} + e.what());
    }
}

} // namespace luminaria
