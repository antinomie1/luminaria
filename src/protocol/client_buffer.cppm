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

/// Give a client-owned wl_buffer its protocol-neutral contents. The resource
/// takes ownership and deletes `buffer` when the client destroys the object.
void install_client_buffer(wl_resource* resource, std::unique_ptr<ClientBuffer> buffer);

/// Contents previously installed on `resource`, or null for shm/foreign buffers.
[[nodiscard]] ClientBuffer* client_buffer_from_resource(wl_resource* resource) noexcept;

} // namespace luminaria

// --------------------------------------------------------------- implementation

namespace luminaria {
namespace {

void client_buffer_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void client_buffer_resource_destroy(wl_resource* resource) {
    delete static_cast<ClientBuffer*>(wl_resource_get_user_data(resource));
}

constexpr struct wl_buffer_interface client_buffer_impl = {
    .destroy = client_buffer_destroy_request,
};

} // namespace

void install_client_buffer(wl_resource* resource, std::unique_ptr<ClientBuffer> buffer) {
    wl_resource_set_implementation(resource, &client_buffer_impl, buffer.release(),
                                   client_buffer_resource_destroy);
}

ClientBuffer* client_buffer_from_resource(wl_resource* resource) noexcept {
    if (resource == nullptr ||
        !wl_resource_instance_of(resource, &wl_buffer_interface, &client_buffer_impl)) {
        return nullptr;
    }
    return static_cast<ClientBuffer*>(wl_resource_get_user_data(resource));
}

} // namespace luminaria
