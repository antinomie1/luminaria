// luminaria/linux_dmabuf.cppm — the zwp_linux_dmabuf_v1 global (GPU client buffers).
//
// Clients (Mesa/EGL, GTK4, …) hand the compositor dmabuf-backed wl_buffers
// instead of copying pixels through shm. A GBM device on a DRM render node is
// the allocator/validator: it tells us which formats the GPU supports.
//
// LINEAR buffers are mmap'd and read on the CPU (fast path). Any other modifier
// the GPU supports is imported through Vulkan external-memory (see
// VulkanRenderer::import_dmabuf) — pass a renderer to enable that and to
// advertise the GPU's real modifier list. Without a renderer we advertise
// LINEAR only.
//
// Importing luminaria pulls in no libwayland headers: the C types this
// interface names are forward-declared in the global module fragment.

module;

#include "luminaria/detail/wayland_fwd.h"

#include <cstdint>
#include <memory>
#include <vector>

export module luminaria:linux_dmabuf;

import :core.expected;
import :util.dmabuf;

export namespace luminaria {

class Display;
class VulkanRenderer;

/// The zwp_linux_dmabuf_v1 global (protocol version 3). Move-only; pointer-stable
/// state so the libwayland global can hold a pointer to it.
class LinuxDmabuf {
public:
    /// Open a GBM device (default DRM render node) and create the global. If
    /// `renderer` is given, advertise its supported DRM modifiers and import
    /// non-LINEAR buffers through it; otherwise advertise LINEAR only. Fails if no
    /// render node is usable.
    [[nodiscard]] static Result<LinuxDmabuf> create(Display& display,
                                                    VulkanRenderer* renderer = nullptr);

    ~LinuxDmabuf();
    LinuxDmabuf(LinuxDmabuf&&) noexcept;
    LinuxDmabuf& operator=(LinuxDmabuf&&) noexcept;
    LinuxDmabuf(const LinuxDmabuf&) = delete;
    LinuxDmabuf& operator=(const LinuxDmabuf&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit LinuxDmabuf(std::unique_ptr<Impl> impl) noexcept;
};

/// If `buffer` is a dmabuf wl_buffer minted by this global, decode it to
/// tightly-packed RGBA8 in `out` (setting width/height); returns true. LINEAR
/// buffers are mmap'd; others go through the renderer stored at creation. Returns
/// false for any other buffer (the caller falls back to shm). The compositor's
/// render bridge calls this when wl_shm_buffer_get() comes back null.
[[nodiscard]] bool dmabuf_buffer_to_rgba(wl_resource* buffer, std::vector<std::uint8_t>& out,
                                         int& width, int& height);

/// Plane + format metadata for a dmabuf wl_buffer minted by this global. Used by
/// screencopy to write a captured frame into a client's dmabuf target, and by
/// the renderer to import it as a GPU texture without a CPU round trip.
using DmabufInfo = DmabufPlane;

/// Fill `out` if `buffer` is a dmabuf wl_buffer from this global; else false.
[[nodiscard]] bool dmabuf_buffer_info(wl_resource* buffer, DmabufInfo& out);

} // namespace luminaria
