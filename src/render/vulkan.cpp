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
        vk::DeviceCreateInfo device_info{{}, queue_info};
        vk::raii::Device device{physical, device_info};
        vk::raii::Queue queue{device, queue_family, 0};
        vk::raii::CommandPool command_pool{
            device, vk::CommandPoolCreateInfo{vk::CommandPoolCreateFlagBits::eTransient, queue_family}};

        auto impl = std::make_unique<Impl>(Impl{std::move(context), std::move(instance),
                                                std::move(physical), queue_family, std::move(device),
                                                std::move(queue), std::move(command_pool)});
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
            // TODO: no clipping — surfaces fully inside the output only.
            if (t.rgba == nullptr || t.w <= 0 || t.h <= 0 || t.x < 0 || t.y < 0 ||
                t.x + t.w > width || t.y + t.h > height) {
                continue;
            }
            const vk::DeviceSize size = static_cast<vk::DeviceSize>(t.w) * t.h * 4;
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
            void* p = sm.mapMemory(0, VK_WHOLE_SIZE);
            std::memcpy(p, t.rgba, static_cast<size_t>(size));
            sm.unmapMemory();

            vk::BufferImageCopy copy{0,
                                     0,
                                     0,
                                     {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                     {t.x, t.y, 0},
                                     {static_cast<uint32_t>(t.w), static_cast<uint32_t>(t.h), 1}};
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

} // namespace luminaria
