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

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "linux-dmabuf-unstable-v1-protocol.h"

export module luminaria.gpu:linux_dmabuf;

import std;

import luminaria;
import :vulkan;

export namespace luminaria {

/// The zwp_linux_dmabuf_v1 global (protocol version 4). Move-only; pointer-stable
/// state so the libwayland global can hold a pointer to it.
///
/// Version 4 matters far more than the version number suggests: `main_device` in
/// the feedback object is the ONLY way left for a client to learn which DRM node
/// to open. Mesa's EGL/Vulkan Wayland platform used to get that from `wl_drm`,
/// which is gone; without feedback it initialises with fd -1, fails to create a
/// DRI screen, and every GL client silently falls back to software rendering.
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
// Implements zwp_linux_dmabuf_v1 (version 4): clients hand us dmabuf-backed
// wl_buffers. LINEAR buffers are mmap'd on the CPU (fast path); any other
// modifier the GPU supports is imported through Vulkan external-memory. We
// advertise the modifier list the renderer reports (LINEAR always included) —
// through the v4 feedback object's format table, or the v3 format/modifier
// events for a client that binds older.

namespace luminaria {

// Lifted out of the anonymous namespace below: FormatMods names it in a
// member signature, and a module-linkage declaration may not expose a
// TU-local type. Still unexported — private to module luminaria.
struct FormatMods {
    uint32_t format;
    std::vector<uint64_t> modifiers;
};

/// One mmap, unmapped by the destructor. Only used while the format table is
/// being written — the compositor has no reason to keep the mapping alive once
/// the memfd is sealed, and a mapping that outlives its writer is a leak that
/// only shows up under pressure.
class DmabufMapping {
    void* addr_ = MAP_FAILED;
    std::size_t size_ = 0;

public:
    DmabufMapping() noexcept = default;
    DmabufMapping(int fd, std::size_t size) noexcept
        : addr_(size == 0 ? MAP_FAILED
                          : mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)),
          size_(size) {}

    ~DmabufMapping() { reset(); }

    DmabufMapping(DmabufMapping&& o) noexcept
        : addr_(std::exchange(o.addr_, MAP_FAILED)), size_(std::exchange(o.size_, 0)) {}
    DmabufMapping& operator=(DmabufMapping&& o) noexcept {
        if (this != &o) {
            reset();
            addr_ = std::exchange(o.addr_, MAP_FAILED);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }
    DmabufMapping(const DmabufMapping&) = delete;
    DmabufMapping& operator=(const DmabufMapping&) = delete;

    [[nodiscard]] bool valid() const noexcept { return addr_ != MAP_FAILED; }
    [[nodiscard]] void* data() const noexcept { return valid() ? addr_ : nullptr; }

    void reset() noexcept {
        if (addr_ != MAP_FAILED) {
            munmap(addr_, size_);
            addr_ = MAP_FAILED;
        }
        size_ = 0;
    }
};

// Lifted out of the anonymous namespace below: GlobalData names it in a
// member signature, and a module-linkage declaration may not expose a
// TU-local type. Still unexported — private to module luminaria.
/// The `format_table` a v4 feedback object hands to clients: a sealed memfd of
/// 16-byte { format, padding, modifier } entries, which the client mmaps
/// read-only and indexes with the u16s in `tranche_formats`. Built once at
/// startup and shared by every feedback resource — the fd is only ever
/// duplicated across the wire by libwayland, never handed away.
class DmabufFormatTable {
    UniqueFd fd_;
    std::uint32_t size_ = 0;
    std::vector<std::uint16_t> indices_; // one tranche: every entry, in order

public:
    struct Entry {
        std::uint32_t format;
        std::uint32_t padding; // must be zeroed: the protocol reserves it
        std::uint64_t modifier;
    };
    static_assert(sizeof(Entry) == 16, "zwp_linux_dmabuf_v1 fixes the entry at 16 bytes");

    [[nodiscard]] bool valid() const noexcept { return fd_.valid(); }
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] const std::vector<std::uint16_t>& indices() const noexcept { return indices_; }

    /// Flatten `formats` into a sealed memfd. An empty table on failure: a
    /// compositor that cannot build one still runs, clients just get no
    /// feedback (and fall back to the v3 format/modifier events).
    static DmabufFormatTable build(const std::vector<FormatMods>& formats) {
        std::vector<Entry> entries;
        for (const FormatMods& fm : formats) {
            for (uint64_t mod : fm.modifiers) {
                entries.push_back(Entry{fm.format, 0, mod});
            }
        }
        DmabufFormatTable table;
        if (entries.empty()) {
            return table;
        }
        // `tranche_formats` indexes the table with u16s, so an entry past 65535
        // is unreachable by any client and must not be written either.
        entries.resize(std::min<std::size_t>(entries.size(), 0xffff));

        const std::size_t bytes = entries.size() * sizeof(Entry);
        UniqueFd fd{memfd_create("luminaria-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING)};
        if (!fd.valid() || ftruncate(fd.get(), static_cast<off_t>(bytes)) != 0) {
            return table;
        }
        {
            const DmabufMapping map{fd.get(), bytes};
            if (!map.valid()) {
                return table;
            }
            std::memcpy(map.data(), entries.data(), bytes);
        } // unmapped here, before the seals go on

        // Sealed so a client that maps it cannot grow, shrink or write it. The
        // protocol requires the table be read-only to clients, and a seal is
        // the only thing that actually enforces that on a shared memfd.
        if (fcntl(fd.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0) {
            return table;
        }

        table.indices_.resize(entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i) {
            table.indices_[i] = static_cast<std::uint16_t>(i);
        }
        table.size_ = static_cast<std::uint32_t>(bytes);
        table.fd_ = std::move(fd);
        return table;
    }
};

// Lifted out of the anonymous namespace below: GlobalData names it in a
// member signature, and a module-linkage declaration may not expose a
// TU-local type. Still unexported — private to module luminaria.
// wl_global user_data: the allocator, the renderer, and the advertised list.
struct GlobalData {
    gbm_device* gbm = nullptr;
    VulkanRenderer* renderer = nullptr;
    std::vector<FormatMods> formats;
    DmabufFormatTable table;
    dev_t main_device = 0; // the render node clients are told to open
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
struct DmabufBuffer final : ClientBuffer {
    int fd = -1;
    int32_t width_value = 0;
    int32_t height_value = 0;
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

    [[nodiscard]] int width() const noexcept override { return width_value; }
    [[nodiscard]] int height() const noexcept override { return height_value; }
    [[nodiscard]] bool rgba(std::vector<std::uint8_t>& out, int& width,
                            int& height) const override;
    [[nodiscard]] bool dmabuf(DmabufPlane& out) const noexcept override;
};

// --- zwp_linux_buffer_params_v1: accumulates planes, then mints a wl_buffer. ---

struct Params {
    bool have_plane0 = false;
    bool used = false;
    VulkanRenderer* renderer = nullptr;
    DmabufBuffer plane0; // only plane_idx 0 is kept (single-plane ARGB/XRGB)
};

Params* params_of(wl_resource* r) { return static_cast<Params*>(wl_resource_get_user_data(r)); }

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
    // `add` took the stride on trust — the protocol has no validation for it at
    // all. This is the one chokepoint both consumers pass through (the CPU
    // mmap path below and VulkanRenderer::import_texture), so a stride too
    // short for a row of `width` 4-byte pixels is refused here and neither has
    // to defend itself against a layout that never existed.
    if (!layout_fits(width, height, p->plane0.stride, p->plane0.offset)) {
        return fail(ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
                    "stride too small for width, or offset/stride out of range");
    }

    wl_resource* buffer = wl_resource_create(client, &wl_buffer_interface, 1, buffer_id);
    if (buffer == nullptr) {
        wl_client_post_no_memory(client);
        return nullptr;
    }
    // Move plane0's owned fd into a heap DmabufBuffer; params keeps none.
    auto buf = std::make_unique<DmabufBuffer>();
    buf->fd = p->plane0.fd;
    buf->width_value = width;
    buf->height_value = height;
    buf->format = format;
    buf->offset = p->plane0.offset;
    buf->stride = p->plane0.stride;
    buf->modifier = p->plane0.modifier;
    buf->renderer = p->renderer;
    p->plane0.fd = -1;
    p->have_plane0 = false;
    install_client_buffer(buffer, std::move(buf));
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
    .destroy = resource_destroy_request,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};

void params_resource_destroy(wl_resource* resource) { delete params_of(resource); }

// --- zwp_linux_dmabuf_v1 global ---

GlobalData* global_of(wl_resource* r) { return static_cast<GlobalData*>(wl_resource_get_user_data(r)); }

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

// --- zwp_linux_dmabuf_feedback_v1 (v4) ---

constexpr struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
    .destroy = resource_destroy_request,
};

/// A `wl_array` holding exactly one `dev_t`, as `main_device` and
/// `tranche_target_device` both want it.
struct DeviceArray {
    wl_array array{};
    explicit DeviceArray(dev_t device) {
        wl_array_init(&array);
        if (void* slot = wl_array_add(&array, sizeof(dev_t)); slot != nullptr) {
            std::memcpy(slot, &device, sizeof(dev_t));
        }
    }
    ~DeviceArray() { wl_array_release(&array); }
    DeviceArray(const DeviceArray&) = delete;
    DeviceArray& operator=(const DeviceArray&) = delete;
};

/// Our feedback never changes after startup — one device, one tranche, every
/// format the GPU reported — so the whole sequence goes out at creation and the
/// object then just sits there until the client destroys it.
void send_feedback(wl_resource* resource, const GlobalData& g) {
    if (!g.table.valid()) {
        // Nothing to describe. `all_done` still has to go out, or a client that
        // is waiting on a roundtrip for it hangs forever.
        zwp_linux_dmabuf_feedback_v1_send_done(resource);
        return;
    }
    zwp_linux_dmabuf_feedback_v1_send_format_table(resource, g.table.fd(), g.table.size());
    {
        DeviceArray main{g.main_device};
        zwp_linux_dmabuf_feedback_v1_send_main_device(resource, &main.array);
    }

    // One tranche, targeting the same device, with no flags: we composite
    // everything through the renderer, so there is no scanout-only subset to
    // promote. `SCANOUT` would be a lie unless the tranche were built from what
    // KMS actually accepts on a plane.
    {
        DeviceArray target{g.main_device};
        zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(resource, &target.array);
    }
    {
        wl_array formats{};
        wl_array_init(&formats);
        const std::size_t bytes = g.table.indices().size() * sizeof(std::uint16_t);
        if (void* slot = wl_array_add(&formats, bytes); slot != nullptr) {
            std::memcpy(slot, g.table.indices().data(), bytes);
        }
        zwp_linux_dmabuf_feedback_v1_send_tranche_formats(resource, &formats);
        wl_array_release(&formats);
    }
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(resource, 0);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(resource);
    zwp_linux_dmabuf_feedback_v1_send_done(resource);
}

void create_feedback(wl_client* client, wl_resource* parent, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
                                               wl_resource_get_version(parent), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &feedback_impl, nullptr, nullptr);
    send_feedback(resource, *global_of(parent));
}

void dmabuf_get_default_feedback(wl_client* client, wl_resource* resource, uint32_t id) {
    create_feedback(client, resource, id);
}

// Per-surface feedback may legally equal the default feedback, and ours does:
// there is one GPU, one tranche, and no per-surface scanout promotion to
// report. The surface is accepted and ignored rather than refused.
void dmabuf_get_surface_feedback(wl_client* client, wl_resource* resource, uint32_t id,
                                 wl_resource* /*surface*/) {
    create_feedback(client, resource, id);
}

constexpr struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = resource_destroy_request,
    .create_params = dmabuf_create_params,
    .get_default_feedback = dmabuf_get_default_feedback,
    .get_surface_feedback = dmabuf_get_surface_feedback,
};

void dmabuf_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource =
        wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &dmabuf_impl, data, nullptr);

    // v4 replaced these with the feedback object and forbids sending them; a
    // v4 client learns the formats from the format table instead.
    if (version >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) {
        return;
    }
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
    UniqueFd drm_fd;
    GlobalData data;
    WlGlobal global;

    // gbm is the one thing here that is not RAII (a C handle with no wrapper of
    // its own). It goes in the body, which runs before any member destructor —
    // so the fd it was created from is still open at this point.
    ~Impl() {
        if (data.gbm != nullptr) {
            gbm_device_destroy(data.gbm);
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
        UniqueFd fd{open(path.c_str(), O_RDWR | O_CLOEXEC)};
        if (!fd.valid()) {
            continue;
        }
        gbm_device* dev = gbm_create_device(fd.get());
        if (dev == nullptr) {
            continue; // fd closed by UniqueFd on the way round the loop
        }
        impl->drm_fd = std::move(fd);
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

    // The whole point of v4: `main_device` is how a client learns which DRM node
    // to open. `st_rdev` of the render node we already hold is exactly that, and
    // without it Mesa initialises EGL with fd -1 and drops to software.
    if (struct stat info {}; fstat(impl->drm_fd.get(), &info) == 0) {
        impl->data.main_device = info.st_rdev;
    }
    impl->data.table = DmabufFormatTable::build(impl->data.formats);

    // Bind at 4 only when there is something for a v4 client to read; a
    // feedback object with no format table is worse than none, because the
    // client stops looking at the v3 events it would otherwise have used.
    const uint32_t version = (impl->data.table.valid() && impl->data.main_device != 0) ? 4 : 3;
    auto global =
        create_wl_global<&zwp_linux_dmabuf_v1_interface, dmabuf_bind>(display, version, &impl->data);
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return LinuxDmabuf{std::move(impl)};
}

namespace {
// mmap a LINEAR dmabuf and convert to RGBA. Returns false on mmap failure.
bool linear_to_rgba(const DmabufBuffer* buf, std::vector<std::uint8_t>& out, int& width,
                    int& height) {
    const int w = buf->width_value;
    const int h = buf->height_value;
    // build_buffer() already refused a short stride; check again rather than
    // trust that every future path into a DmabufBuffer came through it.
    const size_t map_len = layout_length(w, h, buf->stride, buf->offset);
    if (map_len == 0) {
        return false;
    }
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
    return dynamic_cast<const DmabufBuffer*>(client_buffer_from_resource(buffer));
}
} // namespace

bool DmabufBuffer::rgba(std::vector<std::uint8_t>& out, int& width, int& height) const {
    if (is_linear(modifier)) {
        return linear_to_rgba(this, out, width, height);
    }
    if (renderer == nullptr) {
        return false;
    }
    auto pixels = renderer->import_dmabuf(fd, width_value, height_value, format, offset, stride,
                                          modifier);
    if (!pixels) {
        return false;
    }
    out = std::move(*pixels);
    width = width_value;
    height = height_value;
    return true;
}

bool DmabufBuffer::dmabuf(DmabufPlane& out) const noexcept {
    out = DmabufPlane{fd, width_value, height_value, format, offset, stride, modifier};
    return true;
}

bool dmabuf_buffer_to_rgba(wl_resource* buffer, std::vector<std::uint8_t>& out, int& width,
                           int& height) {
    const DmabufBuffer* buf = as_dmabuf(buffer);
    if (buf == nullptr) {
        return false;
    }
    return buf->rgba(out, width, height);
}

bool dmabuf_buffer_info(wl_resource* buffer, DmabufInfo& out) {
    const DmabufBuffer* buf = as_dmabuf(buffer);
    if (buf == nullptr) {
        return false;
    }
    out = DmabufInfo{buf->fd,     buf->width_value,  buf->height_value,  buf->format,
                     buf->offset, buf->stride, buf->modifier};
    return true;
}

} // namespace luminaria
