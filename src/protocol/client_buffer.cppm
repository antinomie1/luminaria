// luminaria/client_buffer.cppm — protocol-neutral wl_buffer contents.
//
// Protocols which mint non-shm wl_buffers install a ClientBuffer on the
// resource. Surface can then ask for size, CPU pixels or dmabuf metadata
// without importing the protocol which produced it. This is the seam that
// keeps the core compositor independent from the GPU module.

module;

#include "detail/wayland_fwd.h"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

export module luminaria:client_buffer;

import std;

import :dmabuf;
import :pixel_layout;

export namespace luminaria {

class ClientBuffer {
public:
    virtual ~ClientBuffer() = default;

    [[nodiscard]] virtual int width() const noexcept = 0;
    [[nodiscard]] virtual int height() const noexcept = 0;

    /// Decode to tightly-packed RGBA8. Unsupported storage returns false.
    [[nodiscard]] virtual bool rgba(std::vector<std::uint8_t>&, int&, int&) const {
        return false;
    }

    /// Describe storage suitable for GPU import or direct scanout.
    [[nodiscard]] virtual bool dmabuf(DmabufPlane&) const noexcept { return false; }
};

struct BufferContentsState;

/// Buffer storage retained independently of the client's wl_buffer resource.
/// Destroying that resource only removes the client's object name: storage
/// already attached to a surface remains valid until the compositor replaces
/// it. Copies share one ref, so pending, cached and current surface state can
/// carry the same contents without copying pixels or duplicating fds.
class BufferContents {
public:
    BufferContents() = default;

    [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] bool rgba(std::vector<std::uint8_t>&, int&, int&) const;
    [[nodiscard]] bool dmabuf(DmabufPlane&) const noexcept;
    [[nodiscard]] const void* identity() const noexcept { return state_.get(); }

private:
    explicit BufferContents(std::shared_ptr<BufferContentsState> state) noexcept
        : state_(std::move(state)) {}
    std::shared_ptr<BufferContentsState> state_;

    friend BufferContents retain_client_buffer(wl_resource* resource);
};

/// Give a client-owned wl_buffer its protocol-neutral contents. The resource
/// owns one reference; surfaces may retain another past resource destruction.
void install_client_buffer(wl_resource* resource, std::unique_ptr<ClientBuffer> buffer);

/// Contents previously installed on `resource`, or null for shm/foreign buffers.
[[nodiscard]] ClientBuffer* client_buffer_from_resource(wl_resource* resource) noexcept;

/// Retain the storage behind a wl_buffer. Empty for an unknown buffer type.
[[nodiscard]] BufferContents retain_client_buffer(wl_resource* resource);

} // namespace luminaria

// --------------------------------------------------------------- implementation

namespace luminaria {

// Lifted out of the anonymous namespace: `std::make_unique<InstalledClientBuffer>`
// is a `tuple<InstalledClientBuffer*, default_delete<...>>`, whose comparisons
// clang instantiates at module scope — a TU-local type in there would be
// ill-formed. Still unexported, so it stays private to module luminaria.
struct InstalledClientBuffer {
    std::shared_ptr<ClientBuffer> contents;
};

namespace {

void client_buffer_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void client_buffer_resource_destroy(wl_resource* resource) {
    delete static_cast<InstalledClientBuffer*>(wl_resource_get_user_data(resource));
}

constexpr struct wl_buffer_interface client_buffer_impl = {
    .destroy = client_buffer_destroy_request,
};

} // namespace

// Namespace scope is deliberate. clang 22.1.8 can ICE while generating an
// unrelated importer when an exported module class owns its nested pimpl type.
struct BufferContentsState {
    wl_shm_buffer* shm = nullptr;
    std::shared_ptr<ClientBuffer> custom;

    explicit BufferContentsState(wl_shm_buffer* buffer) noexcept : shm(buffer) {
        wl_shm_buffer_ref(shm);
    }
    explicit BufferContentsState(std::shared_ptr<ClientBuffer> buffer) noexcept
        : custom(std::move(buffer)) {}
    ~BufferContentsState() {
        if (shm != nullptr) {
            wl_shm_buffer_unref(shm);
        }
    }
};

int BufferContents::width() const noexcept {
    if (state_ == nullptr) {
        return 0;
    }
    return state_->shm != nullptr ? wl_shm_buffer_get_width(state_->shm)
                                  : state_->custom->width();
}

int BufferContents::height() const noexcept {
    if (state_ == nullptr) {
        return 0;
    }
    return state_->shm != nullptr ? wl_shm_buffer_get_height(state_->shm)
                                  : state_->custom->height();
}

bool BufferContents::rgba(std::vector<std::uint8_t>& out, int& width, int& height) const {
    if (state_ == nullptr) {
        return false;
    }
    if (state_->shm == nullptr) {
        return state_->custom->rgba(out, width, height);
    }
    const std::uint32_t format = wl_shm_buffer_get_format(state_->shm);
    if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888) {
        return false;
    }
    const int w = wl_shm_buffer_get_width(state_->shm);
    const int h = wl_shm_buffer_get_height(state_->shm);
    const int stride = wl_shm_buffer_get_stride(state_->shm);
    if (!layout_fits(w, h, stride)) {
        return false;
    }
    const bool opaque = format == WL_SHM_FORMAT_XRGB8888;
    wl_shm_buffer_begin_access(state_->shm);
    const auto* data = static_cast<const std::uint8_t*>(wl_shm_buffer_get_data(state_->shm));
    out.resize(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* src = data + static_cast<std::size_t>(y) * stride;
        std::uint8_t* dst = out.data() + static_cast<std::size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            // shm ARGB8888 is little-endian: bytes are B,G,R,A. Emit RGBA.
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = opaque ? 255 : src[x * 4 + 3];
        }
    }
    wl_shm_buffer_end_access(state_->shm);
    width = w;
    height = h;
    return true;
}

bool BufferContents::dmabuf(DmabufPlane& out) const noexcept {
    return state_ != nullptr && state_->custom != nullptr && state_->custom->dmabuf(out);
}

void install_client_buffer(wl_resource* resource, std::unique_ptr<ClientBuffer> buffer) {
    auto installed = std::make_unique<InstalledClientBuffer>();
    installed->contents = std::shared_ptr<ClientBuffer>{std::move(buffer)};
    wl_resource_set_implementation(resource, &client_buffer_impl, installed.release(),
                                   client_buffer_resource_destroy);
}

ClientBuffer* client_buffer_from_resource(wl_resource* resource) noexcept {
    if (resource == nullptr ||
        !wl_resource_instance_of(resource, &wl_buffer_interface, &client_buffer_impl)) {
        return nullptr;
    }
    auto* installed = static_cast<InstalledClientBuffer*>(wl_resource_get_user_data(resource));
    return installed != nullptr ? installed->contents.get() : nullptr;
}

BufferContents retain_client_buffer(wl_resource* resource) {
    if (resource == nullptr) {
        return {};
    }
    if (wl_shm_buffer* shm = wl_shm_buffer_get(resource); shm != nullptr) {
        return BufferContents{std::make_shared<BufferContentsState>(shm)};
    }
    if (!wl_resource_instance_of(resource, &wl_buffer_interface, &client_buffer_impl)) {
        return {};
    }
    auto* installed = static_cast<InstalledClientBuffer*>(wl_resource_get_user_data(resource));
    return installed != nullptr
               ? BufferContents{std::make_shared<BufferContentsState>(installed->contents)}
               : BufferContents{};
}

} // namespace luminaria
