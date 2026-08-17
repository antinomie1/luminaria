// luminaria/xdg_decoration.cppm — who draws the title bar.
//
// `zxdg_decoration_manager_v1`. A client with a new xdg_toplevel asks whether
// the compositor will decorate it; the compositor answers, and that answer is
// binding — the client draws its own frame only if told to.
//
// Getting this wrong is visible immediately: promise server-side decorations and
// draw none and the window has no title bar at all; stay silent and GTK/Qt
// windows end up with two.

module;


#include <cstdint>
#include <wayland-server-core.h>
#include "xdg-decoration-unstable-v1-protocol.h"

export module luminaria:xdg_decoration;

import std;

import :display;
import :expected;
import :protocol_helper;
import :signal;
import :xdg_shell;

export namespace luminaria {

class Display;
class Toplevel;

/// Values match `zxdg_toplevel_decoration_v1.mode`.
enum class DecorationMode {
    client_side = 1,
    server_side = 2,
};

/// "This window wants to know who decorates it." Set `mode` to decide; whatever
/// it holds when the signal returns is what the client is told. `preferred` is
/// the client's own request, empty when it just wants our default.
struct DecorationRequest {
    Toplevel* toplevel; // null if the resource could not be resolved
    std::optional<DecorationMode> preferred;
    DecorationMode mode; // in: the default; out: the decision
};

class XdgDecorationManager {
public:
    [[nodiscard]] static Result<XdgDecorationManager> create(Display& display);

    ~XdgDecorationManager();
    XdgDecorationManager(XdgDecorationManager&&) noexcept;
    XdgDecorationManager& operator=(XdgDecorationManager&&) noexcept;
    XdgDecorationManager(const XdgDecorationManager&) = delete;
    XdgDecorationManager& operator=(const XdgDecorationManager&) = delete;

    /// What clients are told when nothing listens to `request()`.
    ///
    /// The default is `client_side`, and deliberately: this library draws no
    /// decorations, so a compositor built on it that hasn't thought about the
    /// question is better off letting toolkits draw their own frame than
    /// leaving every window without one.
    void set_default_mode(DecorationMode mode) noexcept;
    [[nodiscard]] DecorationMode default_mode() const noexcept;

    /// Fires per decoration object, on creation and on every set_mode /
    /// unset_mode. Answer by writing to `DecorationRequest::mode`.
    [[nodiscard]] Signal<DecorationRequest>& request() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit XdgDecorationManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct XdgDecorationManager::Impl {
    WlGlobal global;
    Signal<DecorationRequest> request;
    DecorationMode default_mode = DecorationMode::client_side;
};

namespace {

using XdMgr = XdgDecorationManager::Impl;

/// One zxdg_toplevel_decoration_v1. Owned by its resource.
struct Decoration {
    XdMgr* mgr = nullptr;
    Toplevel* toplevel = nullptr;
    wl_resource* resource = nullptr;
    Decoration(XdMgr* m, Toplevel* t, wl_resource* r) : mgr(m), toplevel(t), resource(r) {}
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
    .destroy = resource_destroy_request,
    .set_mode = deco_set_mode,
    .unset_mode = deco_unset_mode,
};

void manager_get_decoration(wl_client* client, wl_resource* resource, uint32_t id,
                            wl_resource* toplevel_resource) {
    auto* mgr = static_cast<XdMgr*>(wl_resource_get_user_data(resource));
    wl_resource* deco_resource =
        wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface,
                           wl_resource_get_version(resource), id);
    if (deco_resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto deco = std::make_unique<Decoration>(mgr, toplevel_from_resource(toplevel_resource), deco_resource);
    Decoration* raw = deco.release();
    wl_resource_set_implementation(deco_resource, &decoration_impl, raw, [](wl_resource* r) {
        delete static_cast<Decoration*>(wl_resource_get_user_data(r));
    });
    // The protocol requires an initial configure so the client knows what to
    // draw before it ever attaches a buffer.
    arbitrate(raw, std::nullopt);
}

constexpr struct zxdg_decoration_manager_v1_interface manager_impl = {
    .destroy = resource_destroy_request,
    .get_toplevel_decoration = manager_get_decoration,
};

} // namespace

XdgDecorationManager::XdgDecorationManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
XdgDecorationManager::~XdgDecorationManager() = default;
XdgDecorationManager::XdgDecorationManager(XdgDecorationManager&&) noexcept = default;
XdgDecorationManager& XdgDecorationManager::operator=(XdgDecorationManager&&) noexcept = default;

Result<XdgDecorationManager> XdgDecorationManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    auto global = create_wl_global<&zxdg_decoration_manager_v1_interface,
                                   default_bind<&zxdg_decoration_manager_v1_interface,
                                                &manager_impl>>(display, 1, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
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
