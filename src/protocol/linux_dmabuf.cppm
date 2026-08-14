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

#include "detail/wayland_fwd.h"
#include <cstdint>
#include <memory>
#include <vector>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "linux-dmabuf-unstable-v1-protocol.h"

export module luminaria:linux_dmabuf;

import :display;
import :dmabuf;
import :expected;
import :vulkan;

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

// --------------------------------------------------------------- implementation
// Implements zwp_linux_dmabuf_v1 (version 3): clients hand us dmabuf-backed
// wl_buffers. LINEAR buffers are mmap'd on the CPU (fast path); any other
// modifier the GPU supports is imported through Vulkan external-memory. We
// advertise the modifier list the renderer reports (LINEAR always included).

namespace luminaria {

// Lifted out of the anonymous namespace below: FormatMods names it in a
// member signature, and a module-linkage declaration may not expose a
// TU-local type. Still unexported — private to module luminaria.
struct FormatMods {
    uint32_t format;
    std::vector<uint64_t> modifiers;
};

// Lifted out of the anonymous namespace below: GlobalData names it in a
// member signature, and a module-linkage declaration may not expose a
// TU-local type. Still unexported — private to module luminaria.
// wl_global user_data: the allocator, the renderer, and the advertised list.
struct GlobalData {
    gbm_device* gbm = nullptr;
    VulkanRenderer* renderer = nullptr;
    std::vector<FormatMods> formats;
};

namespace {

// Formats we advertise + can convert. DRM fourcc.
constexpr std::array<uint32_t, 2> kFormats{DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888};

bool supported_format(uint32_t fourcc) {
    return fourcc == DRM_FORMAT_ARGB8888 || fourcc == DRM_FORMAT_XRGB8888;
}

uint64_t join_modifier(uint32_t hi, uint32_t lo) {
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// A LINEAR/implicit modifier is CPU-mappable; anything else goes via the GPU.
bool is_linear(uint64_t modifier) {
    return modifier == DRM_FORMAT_MOD_LINEAR || modifier == DRM_FORMAT_MOD_INVALID;
}

// One imported dmabuf, owned by its wl_buffer resource. Single plane.
struct DmabufBuffer {
    int fd = -1;
    int32_t width = 0;
    int32_t height = 0;
    uint32_t format = 0;
    uint32_t offset = 0;
    uint32_t stride = 0;
    uint64_t modifier = DRM_FORMAT_MOD_INVALID;
    VulkanRenderer* renderer = nullptr; // for non-LINEAR import

    ~DmabufBuffer() {
        if (fd >= 0) {
            close(fd);
        }
    }
};

void buffer_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
constexpr struct wl_buffer_interface dmabuf_wl_buffer_impl = {
    .destroy = buffer_destroy,
};
void buffer_resource_destroy(wl_resource* resource) {
    delete static_cast<DmabufBuffer*>(wl_resource_get_user_data(resource));
}

// --- zwp_linux_buffer_params_v1: accumulates planes, then mints a wl_buffer. ---

struct Params {
    bool have_plane0 = false;
    bool used = false;
    VulkanRenderer* renderer = nullptr;
    DmabufBuffer plane0; // only plane_idx 0 is kept (single-plane ARGB/XRGB)
};

Params* params_of(wl_resource* r) { return static_cast<Params*>(wl_resource_get_user_data(r)); }

void params_destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void params_add(wl_client*, wl_resource* resource, int32_t fd, uint32_t plane_idx, uint32_t offset,
                uint32_t stride, uint32_t modifier_hi, uint32_t modifier_lo) {
    Params* p = params_of(resource);
    if (plane_idx != 0) {
        // Multi-plane (YUV etc.) unsupported: we only composite packed RGB. Drop
        // the extra plane's fd so it isn't leaked.
        close(fd);
        return;
    }
    if (p->have_plane0) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                               "plane 0 already set");
        close(fd);
        return;
    }
    p->plane0.fd = fd;
    p->plane0.offset = offset;
    p->plane0.stride = stride;
    p->plane0.modifier = join_modifier(modifier_hi, modifier_lo);
    p->have_plane0 = true;
}

// Shared create/create_immed body. Returns the new wl_buffer, or nullptr after
// posting an error / sending `failed`.
wl_resource* build_buffer(wl_client* client, wl_resource* params_resource, uint32_t buffer_id,
                          int32_t width, int32_t height, uint32_t format, bool immed) {
    Params* p = params_of(params_resource);
    auto fail = [&](uint32_t code, const char* msg) -> wl_resource* {
        if (immed) {
            wl_resource_post_error(params_resource, code, "%s", msg);
        } else {
            zwp_linux_buffer_params_v1_send_failed(params_resource);
        }
        return nullptr;
    };

    if (p->used) {
        return fail(ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "params already used");
    }
    p->used = true;
    if (!p->have_plane0) {
        return fail(ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "no planes added");
    }
    if (!supported_format(format)) {
        return fail(ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT, "unsupported format");
    }
    if (width <= 0 || height <= 0) {
        return fail(ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS, "invalid dimensions");
    }

    wl_resource* buffer = wl_resource_create(client, &wl_buffer_interface, 1, buffer_id);
    if (buffer == nullptr) {
        wl_client_post_no_memory(client);
        return nullptr;
    }
    // Move plane0's owned fd into a heap DmabufBuffer; params keeps none.
    auto* buf = new DmabufBuffer{p->plane0.fd,     width,  height,        format,
                                 p->plane0.offset, p->plane0.stride, p->plane0.modifier,
                                 p->renderer};
    p->plane0.fd = -1;
    p->have_plane0 = false;
    wl_resource_set_implementation(buffer, &dmabuf_wl_buffer_impl, buf, buffer_resource_destroy);
    return buffer;
}

void params_create(wl_client* client, wl_resource* resource, int32_t width, int32_t height,
                   uint32_t format, uint32_t /*flags*/) {
    wl_resource* buffer = build_buffer(client, resource, 0, width, height, format, /*immed=*/false);
    if (buffer != nullptr) {
        zwp_linux_buffer_params_v1_send_created(resource, buffer);
    }
}

void params_create_immed(wl_client* client, wl_resource* resource, uint32_t buffer_id,
                         int32_t width, int32_t height, uint32_t format, uint32_t /*flags*/) {
    build_buffer(client, resource, buffer_id, width, height, format, /*immed=*/true);
}

constexpr struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy = params_destroy,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};

void params_resource_destroy(wl_resource* resource) { delete params_of(resource); }

// --- zwp_linux_dmabuf_v1 global ---

GlobalData* global_of(wl_resource* r) { return static_cast<GlobalData*>(wl_resource_get_user_data(r)); }

void dmabuf_destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void dmabuf_create_params(wl_client* client, wl_resource* resource, uint32_t params_id) {
    wl_resource* r = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                                        wl_resource_get_version(resource), params_id);
    if (r == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* params = new Params{};
    params->renderer = global_of(resource)->renderer;
    wl_resource_set_implementation(r, &params_impl, params, params_resource_destroy);
}

constexpr struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = dmabuf_destroy,
    .create_params = dmabuf_create_params,
    // get_default_feedback / get_surface_feedback are v4+; we bind at v3.
};

void dmabuf_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource =
        wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &dmabuf_impl, data, nullptr);

    // Advertise every (format, modifier) pair (v3 `modifier` event; also the
    // legacy `format` event for v1/v2 fallback clients).
    auto* g = static_cast<GlobalData*>(data);
    for (const FormatMods& fm : g->formats) {
        zwp_linux_dmabuf_v1_send_format(resource, fm.format);
        for (uint64_t mod : fm.modifiers) {
            zwp_linux_dmabuf_v1_send_modifier(resource, fm.format,
                                              static_cast<uint32_t>(mod >> 32),
                                              static_cast<uint32_t>(mod));
        }
    }
}

} // namespace

struct LinuxDmabuf::Impl {
    int drm_fd = -1;
    GlobalData data;
    wl_global* global = nullptr;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
        if (data.gbm != nullptr) {
            gbm_device_destroy(data.gbm);
        }
        if (drm_fd >= 0) {
            close(drm_fd);
        }
    }
};

LinuxDmabuf::LinuxDmabuf(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LinuxDmabuf::~LinuxDmabuf() = default;
LinuxDmabuf::LinuxDmabuf(LinuxDmabuf&&) noexcept = default;
LinuxDmabuf& LinuxDmabuf::operator=(LinuxDmabuf&&) noexcept = default;

Result<LinuxDmabuf> LinuxDmabuf::create(Display& display, VulkanRenderer* renderer) {
    auto impl = std::make_unique<Impl>();

    // Open a DRM render node (unprivileged; the GBM allocator/validator device).
    for (int i = 128; i < 128 + 16; ++i) {
        std::string path = "/dev/dri/renderD" + std::to_string(i);
        int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        gbm_device* dev = gbm_create_device(fd);
        if (dev == nullptr) {
            close(fd);
            continue;
        }
        impl->drm_fd = fd;
        impl->data.gbm = dev;
        break;
    }
    if (impl->data.gbm == nullptr) {
        return fail("linux-dmabuf: no usable DRM render node");
    }
    impl->data.renderer = (renderer != nullptr && renderer->dmabuf_supported()) ? renderer : nullptr;

    // Build the advertised (format, modifiers) list: the GPU's modifiers when a
    // renderer is present, LINEAR otherwise. LINEAR is always offered (mmap path).
    for (uint32_t fourcc : kFormats) {
        if (!gbm_device_is_format_supported(impl->data.gbm, fourcc, GBM_BO_USE_RENDERING)) {
            continue;
        }
        std::vector<uint64_t> mods;
        if (impl->data.renderer != nullptr) {
            mods = impl->data.renderer->dmabuf_modifiers(fourcc);
        }
        if (std::find(mods.begin(), mods.end(), uint64_t{DRM_FORMAT_MOD_LINEAR}) == mods.end()) {
            mods.push_back(DRM_FORMAT_MOD_LINEAR);
        }
        impl->data.formats.push_back(FormatMods{fourcc, std::move(mods)});
    }

    impl->global = wl_global_create(display.c_ptr(), &zwp_linux_dmabuf_v1_interface, 3, &impl->data,
                                    dmabuf_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(zwp_linux_dmabuf_v1) failed");
    }
    return LinuxDmabuf{std::move(impl)};
}

namespace {
// mmap a LINEAR dmabuf and convert to RGBA. Returns false on mmap failure.
bool linear_to_rgba(const DmabufBuffer* buf, std::vector<std::uint8_t>& out, int& width,
                    int& height) {
    const int w = buf->width;
    const int h = buf->height;
    const size_t map_len = static_cast<size_t>(buf->offset) + static_cast<size_t>(buf->stride) * h;
    // dma-buf mmap requires MAP_SHARED (no private copy-on-write mapping).
    void* map = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, buf->fd, 0);
    if (map == MAP_FAILED) {
        return false;
    }
    const bool opaque = buf->format == DRM_FORMAT_XRGB8888;
    const auto* base = static_cast<const uint8_t*>(map) + buf->offset;
    out.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = base + static_cast<size_t>(y) * buf->stride;
        uint8_t* dst = out.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            // LINEAR ARGB8888 is little-endian: bytes B,G,R,A. Emit RGBA.
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = opaque ? 255 : src[x * 4 + 3];
        }
    }
    munmap(map, map_len);
    width = w;
    height = h;
    return true;
}

const DmabufBuffer* as_dmabuf(wl_resource* buffer) {
    if (buffer == nullptr ||
        !wl_resource_instance_of(buffer, &wl_buffer_interface, &dmabuf_wl_buffer_impl)) {
        return nullptr;
    }
    return static_cast<const DmabufBuffer*>(wl_resource_get_user_data(buffer));
}
} // namespace

bool dmabuf_buffer_to_rgba(wl_resource* buffer, std::vector<std::uint8_t>& out, int& width,
                           int& height) {
    const DmabufBuffer* buf = as_dmabuf(buffer);
    if (buf == nullptr) {
        return false;
    }
    if (is_linear(buf->modifier)) {
        return linear_to_rgba(buf, out, width, height);
    }
    // Tiled/vendor modifier: import through the GPU.
    if (buf->renderer == nullptr) {
        return false;
    }
    auto rgba = buf->renderer->import_dmabuf(buf->fd, buf->width, buf->height, buf->format,
                                             buf->offset, buf->stride, buf->modifier);
    if (!rgba) {
        return false;
    }
    out = std::move(*rgba);
    width = buf->width;
    height = buf->height;
    return true;
}

bool dmabuf_buffer_info(wl_resource* buffer, DmabufInfo& out) {
    const DmabufBuffer* buf = as_dmabuf(buffer);
    if (buf == nullptr) {
        return false;
    }
    out = DmabufInfo{buf->fd,     buf->width,  buf->height,  buf->format,
                     buf->offset, buf->stride, buf->modifier};
    return true;
}

} // namespace luminaria
