module;

#include <memory>

#include <optional>

#include <cstdint>

#include <wayland-server-core.h>

#include "xdg-decoration-unstable-v1-protocol.h"

module luminaria;

namespace luminaria {

struct XdgDecorationManager::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    Signal<DecorationRequest> request;
    DecorationMode default_mode = DecorationMode::client_side;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

namespace {

using Mgr = XdgDecorationManager::Impl;

/// One zxdg_toplevel_decoration_v1. Owned by its resource.
struct Decoration {
    Mgr* mgr = nullptr;
    Toplevel* toplevel = nullptr;
    wl_resource* resource = nullptr;
};

Decoration* decoration_of(wl_resource* r) {
    return static_cast<Decoration*>(wl_resource_get_user_data(r));
}

/// Ask the compositor, then tell the client. The answer is always sent, even
/// when it is the same as last time: the protocol makes configure the reply to
/// set_mode, and a client may be waiting for it.
void arbitrate(Decoration* deco, std::optional<DecorationMode> preferred) {
    DecorationRequest event{deco->toplevel, preferred, deco->mgr->default_mode};
    if (preferred.has_value()) {
        // A client's preference is the starting point; the compositor may
        // override it, and is entitled to.
        event.mode = *preferred;
    }
    deco->mgr->request.emit(event);
    zxdg_toplevel_decoration_v1_send_configure(deco->resource,
                                               static_cast<uint32_t>(event.mode));
}

void deco_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void deco_set_mode(wl_client*, wl_resource* resource, uint32_t mode) {
    if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
        mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
        return; // not a mode we know; leave the current one alone
    }
    arbitrate(decoration_of(resource), static_cast<DecorationMode>(mode));
}
void deco_unset_mode(wl_client*, wl_resource* resource) {
    arbitrate(decoration_of(resource), std::nullopt);
}
constexpr struct zxdg_toplevel_decoration_v1_interface decoration_impl = {
    .destroy = deco_destroy_request,
    .set_mode = deco_set_mode,
    .unset_mode = deco_unset_mode,
};
void decoration_resource_destroy(wl_resource* resource) {
    delete decoration_of(resource);
}

void manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void manager_get_decoration(wl_client* client, wl_resource* resource, uint32_t id,
                            wl_resource* toplevel_resource) {
    auto* mgr = static_cast<Mgr*>(wl_resource_get_user_data(resource));
    wl_resource* deco_resource =
        wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface,
                           wl_resource_get_version(resource), id);
    if (deco_resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* deco = new Decoration{mgr, toplevel_from_resource(toplevel_resource), deco_resource};
    wl_resource_set_implementation(deco_resource, &decoration_impl, deco,
                                   decoration_resource_destroy);
    // The protocol requires an initial configure so the client knows what to
    // draw before it ever attaches a buffer.
    arbitrate(deco, std::nullopt);
}

constexpr struct zxdg_decoration_manager_v1_interface manager_impl = {
    .destroy = manager_destroy_request,
    .get_toplevel_decoration = manager_get_decoration,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zxdg_decoration_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

} // namespace

XdgDecorationManager::XdgDecorationManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
XdgDecorationManager::~XdgDecorationManager() = default;
XdgDecorationManager::XdgDecorationManager(XdgDecorationManager&&) noexcept = default;
XdgDecorationManager& XdgDecorationManager::operator=(XdgDecorationManager&&) noexcept = default;

Result<XdgDecorationManager> XdgDecorationManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->global = wl_global_create(impl->display, &zxdg_decoration_manager_v1_interface, 1,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(zxdg_decoration_manager_v1) failed");
    }
    return XdgDecorationManager{std::move(impl)};
}

void XdgDecorationManager::set_default_mode(DecorationMode mode) noexcept {
    impl_->default_mode = mode;
}

DecorationMode XdgDecorationManager::default_mode() const noexcept {
    return impl_->default_mode;
}

Signal<DecorationRequest>& XdgDecorationManager::request() noexcept {
    return impl_->request;
}

} // namespace luminaria
