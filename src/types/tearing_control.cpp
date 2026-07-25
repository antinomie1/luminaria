module;

// Implements wp_tearing_control_manager_v1 (version 1). The hint is
// double-buffered surface state, so it is stashed as pending and applied by the
// next wl_surface.commit like everything else.

#include <memory>
#include <utility>

#include <wayland-server-core.h>

#include "tearing-control-v1-protocol.h"

module luminaria;

namespace luminaria {

namespace {

// One wp_tearing_control_v1 object. It outlives nothing: if the surface goes
// first, the subscription clears the pointer and the object turns inert.
struct TearingControl {
    Surface* surface = nullptr;
    Signal<SurfaceDestroy>::Connection on_surface_destroy;
};

void control_set_presentation_hint(wl_client*, wl_resource* resource, uint32_t hint) {
    auto* control = static_cast<TearingControl*>(wl_resource_get_user_data(resource));
    if (control->surface != nullptr) {
        control->surface->set_pending_tearing_hint(hint == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
    }
}

void control_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void control_resource_destroy(wl_resource* resource) {
    auto* control = static_cast<TearingControl*>(wl_resource_get_user_data(resource));
    // Destroying the object drops the hint back to vsync, per the protocol.
    if (control->surface != nullptr) {
        control->surface->set_pending_tearing_hint(false);
    }
    delete control;
}

constexpr struct wp_tearing_control_v1_interface control_impl = {
    .set_presentation_hint = control_set_presentation_hint,
    .destroy = control_destroy_request,
};

void manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

// ponytail: no `tearing_control_exists` protocol error. Catching a duplicate
// needs a surface->control map that survives surface death; a second control
// simply wins instead, which no real client asks for.
void manager_get_tearing_control(wl_client* client, wl_resource* manager, uint32_t id,
                                 wl_resource* surface_resource) {
    Surface* surface = surface_from_resource(surface_resource);
    wl_resource* resource = wl_resource_create(client, &wp_tearing_control_v1_interface,
                                               wl_resource_get_version(manager),
                                               static_cast<int>(id));
    if (resource == nullptr) {
        wl_resource_post_no_memory(manager);
        return;
    }
    auto* control = new TearingControl{surface, {}};
    if (surface != nullptr) {
        control->on_surface_destroy =
            surface->destroy.connect([control](SurfaceDestroy&) { control->surface = nullptr; });
    }
    wl_resource_set_implementation(resource, &control_impl, control, control_resource_destroy);
}

constexpr struct wp_tearing_control_manager_v1_interface manager_impl = {
    .destroy = manager_destroy_request,
    .get_tearing_control = manager_get_tearing_control,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wp_tearing_control_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

} // namespace

struct TearingControlManager::Impl {
    wl_global* global = nullptr;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

TearingControlManager::TearingControlManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
TearingControlManager::~TearingControlManager() = default;
TearingControlManager::TearingControlManager(TearingControlManager&&) noexcept = default;
TearingControlManager& TearingControlManager::operator=(TearingControlManager&&) noexcept = default;

Result<TearingControlManager> TearingControlManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->global = wl_global_create(display.c_ptr(), &wp_tearing_control_manager_v1_interface, 1,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wp_tearing_control_manager_v1) failed");
    }
    return TearingControlManager{std::move(impl)};
}

} // namespace luminaria
