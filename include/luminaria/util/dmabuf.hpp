// luminaria/util/dmabuf.hpp — one single-plane DMA-BUF, described.
//
// The neutral currency between everything that passes GPU memory around without
// copying it: linux-dmabuf client buffers, the Vulkan renderer's scanout
// targets, and the KMS framebuffers a DRM output scans out. Plain data, no
// ownership — `fd` is always borrowed (dup it if you need to keep it).
#pragma once

#include <cstdint>

namespace luminaria {

struct DmabufPlane {
    int fd = -1;
    int width = 0;
    int height = 0;
    std::uint32_t format = 0; // DRM fourcc
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
    std::uint64_t modifier = 0;
};

} // namespace luminaria
