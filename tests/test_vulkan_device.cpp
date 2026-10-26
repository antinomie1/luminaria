// Physical-device selection: a renderer asked for the DRM device the display
// runs on ends up on it, and one asked for a device that does not exist still
// comes up — on whatever the ranking picks — instead of failing.
//
// Uses a render node, which needs no DRM master. Skips (77) without one or
// without a Vulkan 1.3 device.
#include <cassert>
#include <cstdio>

#include <fcntl.h>
#include <unistd.h>

import luminaria.gpu;
import std;

int main() {
    // Not a device node: nothing to identify.
    assert(!luminaria::drm_device_id(-1).has_value());
    assert(!luminaria::drm_device_id(std::string{"/"}).has_value());

    std::string node;
    for (int minor = 128; minor < 192 && node.empty(); ++minor) {
        const std::string path = "/dev/dri/renderD" + std::to_string(minor);
        if (access(path.c_str(), R_OK) == 0) {
            node = path;
        }
    }
    if (node.empty()) {
        std::fprintf(stderr, "skip: no readable DRM render node\n");
        return 77;
    }

    const auto by_path = luminaria::drm_device_id(node);
    assert(by_path.has_value());
    const int fd = open(node.c_str(), O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    const auto by_fd = luminaria::drm_device_id(fd);
    close(fd);
    assert(by_fd.has_value());
    assert(by_fd->major == by_path->major && by_fd->minor == by_path->minor);

    auto plain = luminaria::VulkanRenderer::create();
    if (!plain) {
        std::fprintf(stderr, "skip: %s\n", plain.error().message.c_str());
        return 77;
    }
    assert(!plain->device_name().empty());
    assert(!plain->on_drm_device()); // nothing was asked for

    auto matched = luminaria::VulkanRenderer::create({by_path});
    assert(matched.has_value());
    if (!matched->on_drm_device()) {
        // A render node with no Vulkan driver behind it (or a driver without
        // VK_EXT_physical_device_drm): the ranking chose, which is the point.
        std::fprintf(stderr, "skip: %s is not a Vulkan device\n", node.c_str());
        return 77;
    }
    std::fprintf(stderr, "%s -> %.*s\n", node.c_str(),
                 static_cast<int>(matched->device_name().size()),
                 matched->device_name().data());

    // A device number no GPU has: still a renderer, just not "on" it.
    auto nowhere = luminaria::VulkanRenderer::create({luminaria::DrmDeviceId{0, 0}});
    assert(nowhere.has_value());
    assert(!nowhere->on_drm_device());

    // And the matched one actually renders.
    auto px = matched->render_clear_readback(8, 8, luminaria::Color{0.0f, 0.0f, 1.0f, 1.0f});
    assert(px.has_value());
    assert((*px == luminaria::Pixel{0, 0, 255, 255}));
    return 0;
}
