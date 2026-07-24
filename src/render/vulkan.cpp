#include "luminaria/render/vulkan.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h> // dup

#include <drm_fourcc.h>
#include <vulkan/vulkan_raii.hpp>

namespace luminaria {

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
VulkanRenderer::~VulkanRenderer() = default;
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

        vk::DeviceCreateInfo device_info{{}, queue_info};
        device_info.setPEnabledExtensionNames(exts);
        vk::raii::Device device{physical, device_info};
        vk::raii::Queue queue{device, queue_family, 0};
        vk::raii::CommandPool command_pool{
            device, vk::CommandPoolCreateInfo{vk::CommandPoolCreateFlagBits::eTransient, queue_family}};

        auto impl = std::make_unique<Impl>(Impl{std::move(context), std::move(instance),
                                                std::move(physical), queue_family, std::move(device),
                                                std::move(queue), std::move(command_pool),
                                                dmabuf_ok, queue_foreign});
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

bool VulkanRenderer::dmabuf_supported() const noexcept { return impl_->dmabuf_ok; }

std::vector<std::uint64_t> VulkanRenderer::dmabuf_modifiers(std::uint32_t drm_format) {
    std::vector<std::uint64_t> out;
    if (!impl_->dmabuf_ok) {
        return out;
    }
    const vk::Format fmt = drm_to_vk(drm_format);
    if (fmt == vk::Format::eUndefined) {
        return out;
    }
    // C structs + core-1.1 entrypoint: two-pass (count, then fill).
    VkDrmFormatModifierPropertiesListEXT list{};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    props2.pNext = &list;
    const VkPhysicalDevice phys = static_cast<VkPhysicalDevice>(*impl_->physical);
    vkGetPhysicalDeviceFormatProperties2(phys, static_cast<VkFormat>(fmt), &props2);
    std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = mods.data();
    vkGetPhysicalDeviceFormatProperties2(phys, static_cast<VkFormat>(fmt), &props2);

    constexpr VkFormatFeatureFlags want =
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (const auto& m : mods) {
        if (m.drmFormatModifierPlaneCount == 1 &&
            (m.drmFormatModifierTilingFeatures & want) == want) {
            out.push_back(m.drmFormatModifier);
        }
    }
    return out;
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

} // namespace luminaria
