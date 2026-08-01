module;

// Implements zwp_relative_pointer_manager_v1 (version 1). There is no state to
// speak of: the manager keeps the live zwp_relative_pointer_v1 objects and
// forwards motion to whichever of them belongs to the pointer-focused client.

#include <memory>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "relative-pointer-unstable-v1-protocol.h"

module luminaria;

namespace luminaria {

struct RelativePointerManager::Impl {
    wl_global* global = nullptr;
    Seat* seat = nullptr;
    std::vector<wl_resource*> relative_pointers;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

namespace {

using RpMgr = RelativePointerManager::Impl;

void relative_pointer_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

constexpr struct zwp_relative_pointer_v1_interface relative_pointer_impl = {
    .destroy = relative_pointer_destroy_request,
};

void relative_pointer_resource_destroy(wl_resource* resource) {
    auto* mgr = static_cast<RpMgr*>(wl_resource_get_user_data(resource));
    std::erase(mgr->relative_pointers, resource);
}

void manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void manager_get_relative_pointer(wl_client* client, wl_resource* manager_resource, uint32_t id,
                                  wl_resource* /*pointer*/) {
    // The wl_pointer argument only says WHICH pointer of the client's; since a
    // client sees one seat's pointer at a time here, its identity adds nothing
    // beyond the client, which the new resource already carries.
    auto* mgr = static_cast<RpMgr*>(wl_resource_get_user_data(manager_resource));
    wl_resource* resource =
        wl_resource_create(client, &zwp_relative_pointer_v1_interface,
                           wl_resource_get_version(manager_resource), static_cast<int>(id));
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &relative_pointer_impl, mgr,
                                   relative_pointer_resource_destroy);
    mgr->relative_pointers.push_back(resource);
}

constexpr struct zwp_relative_pointer_manager_v1_interface manager_impl = {
    .destroy = manager_destroy_request,
    .get_relative_pointer = manager_get_relative_pointer,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zwp_relative_pointer_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

} // namespace

RelativePointerManager::RelativePointerManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
RelativePointerManager::~RelativePointerManager() = default;
RelativePointerManager::RelativePointerManager(RelativePointerManager&&) noexcept = default;
RelativePointerManager&
RelativePointerManager::operator=(RelativePointerManager&&) noexcept = default;

Result<RelativePointerManager> RelativePointerManager::create(Display& display, Seat& seat) {
    auto impl = std::make_unique<Impl>();
    impl->seat = &seat;
    impl->global = wl_global_create(display.c_ptr(), &zwp_relative_pointer_manager_v1_interface, 1,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(zwp_relative_pointer_manager_v1) failed");
    }
    return RelativePointerManager{std::move(impl)};
}

void RelativePointerManager::send_motion(std::uint64_t time_us, double dx, double dy,
                                         double dx_unaccel, double dy_unaccel) {
    Surface* focus = impl_->seat->pointer_focus();
    if (focus == nullptr || impl_->relative_pointers.empty()) {
        return;
    }
    wl_client* client = wl_resource_get_client(focus->c_resource());
    // The timestamp is a 64-bit microsecond value split across two 32-bit
    // arguments, because the protocol predates wayland's 64-bit types.
    const auto hi = static_cast<uint32_t>(time_us >> 32);
    const auto lo = static_cast<uint32_t>(time_us & 0xffffffffU);
    for (wl_resource* rp : impl_->relative_pointers) {
        if (wl_resource_get_client(rp) != client) {
            continue;
        }
        zwp_relative_pointer_v1_send_relative_motion(
            rp, hi, lo, wl_fixed_from_double(dx), wl_fixed_from_double(dy),
            wl_fixed_from_double(dx_unaccel), wl_fixed_from_double(dy_unaccel));
    }
}

} // namespace luminaria
