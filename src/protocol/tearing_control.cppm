// luminaria/tearing_control.cppm — the wp_tearing_control_manager_v1 global.
//
// A client (a game, mostly) says "show my frame the instant it is ready, don't
// wait for vblank". The hint lands on the Surface (`Surface::tearing_hint()`)
// and the compositor decides: it only makes sense for a surface that owns the
// whole output, since an async flip updates the entire scanout buffer.
//
// Importing luminaria pulls in no libwayland headers: the C types this
// interface names are forward-declared in the global module fragment.

module;


#include <wayland-server-core.h>
#include "tearing-control-v1-protocol.h"

export module luminaria:tearing_control;

import std;

import :compositor;
import :display;
import :expected;
import :protocol_helper;
import :signal;

export namespace luminaria {

class Display;

/// The wp_tearing_control_manager_v1 global (version 1). Move-only;
/// pointer-stable state so the libwayland global can hold a pointer to it.
class TearingControlManager {
public:
    [[nodiscard]] static Result<TearingControlManager> create(Display& display);

    ~TearingControlManager();
    TearingControlManager(TearingControlManager&&) noexcept;
    TearingControlManager& operator=(TearingControlManager&&) noexcept;
    TearingControlManager(const TearingControlManager&) = delete;
    TearingControlManager& operator=(const TearingControlManager&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit TearingControlManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Implements wp_tearing_control_manager_v1 (version 1). The hint is
// double-buffered surface state, so it is stashed as pending and applied by the
// next wl_surface.commit like everything else.

namespace luminaria {

namespace {

struct TearingControl : SurfaceTracker {
    using SurfaceTracker::SurfaceTracker;

    ~TearingControl() override {
        if (surface != nullptr) {
            surface->set_pending_tearing_hint(false);
        }
    }
};

void control_set_presentation_hint(wl_client*, wl_resource* resource, uint32_t hint) {
    auto* control = static_cast<TearingControl*>(wl_resource_get_user_data(resource));
    if (control->surface != nullptr) {
        control->surface->set_pending_tearing_hint(hint == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
    }
}

constexpr struct wp_tearing_control_v1_interface control_impl = {
    .set_presentation_hint = control_set_presentation_hint,
    .destroy = resource_destroy_request,
};

void manager_get_tearing_control(wl_client* client, wl_resource* manager, uint32_t id,
                                 wl_resource* surface_resource) {
    Surface* surface = surface_from_resource(surface_resource);
    auto control = std::make_unique<TearingControl>(surface);
    create_user_resource<TearingControl, &wp_tearing_control_v1_interface, &control_impl>(
        client, wl_resource_get_version(manager), id, std::move(control), manager);
}

constexpr struct wp_tearing_control_manager_v1_interface manager_impl = {
    .destroy = resource_destroy_request,
    .get_tearing_control = manager_get_tearing_control,
};

} // namespace

struct TearingControlManager::Impl {
    WlGlobal global;
};

TearingControlManager::TearingControlManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
TearingControlManager::~TearingControlManager() = default;
TearingControlManager::TearingControlManager(TearingControlManager&&) noexcept = default;
TearingControlManager& TearingControlManager::operator=(TearingControlManager&&) noexcept = default;

Result<TearingControlManager> TearingControlManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    auto global = create_wl_global<&wp_tearing_control_manager_v1_interface,
                                   default_bind<&wp_tearing_control_manager_v1_interface,
                                                &manager_impl>>(display, 1, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return TearingControlManager{std::move(impl)};
}

} // namespace luminaria
