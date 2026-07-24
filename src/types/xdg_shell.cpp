#include "luminaria/xdg_shell.hpp"

#include <memory>

#include <wayland-server-core.h>

#include "xdg-shell-protocol.h"

#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"

namespace luminaria {

struct XdgShell::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    Signal<NewToplevel> new_toplevel;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

namespace {

struct XdgSurface;

// Concrete toplevel. Owned by its own xdg_toplevel wl_resource (address stable
// for its lifetime). `owner` links back to the XdgSurface; either side nulls the
// cross-pointer when it dies, so client teardown in any resource order is safe.
class ToplevelImpl final : public Toplevel {
public:
    Surface* surf = nullptr;
    wl_resource* resource = nullptr; // xdg_toplevel
    XdgSurface* owner = nullptr;     // nulled if the xdg_surface is destroyed first
    bool mapped_ = false;

    Surface& surface() noexcept override { return *surf; }
    [[nodiscard]] bool mapped() const noexcept override { return mapped_; }
};

// Per-xdg_surface driver. Owned by the xdg_surface resource.
struct XdgSurface {
    XdgShell::Impl* shell = nullptr;
    wl_resource* resource = nullptr; // xdg_surface
    Surface* surface = nullptr;
    Signal<SurfaceCommit>::Connection commit_conn;
    ToplevelImpl* toplevel = nullptr; // owned by its resource; nulled on its destroy
    bool initialized = false;
};

void send_configure(XdgSurface* xs) {
    if (xs->toplevel && xs->toplevel->resource != nullptr) {
        wl_array states;
        wl_array_init(&states);
        xdg_toplevel_send_configure(xs->toplevel->resource, 0, 0, &states);
        wl_array_release(&states);
    }
    const uint32_t serial = wl_display_next_serial(xs->shell->display);
    xdg_surface_send_configure(xs->resource, serial);
}

void on_surface_commit(XdgSurface* xs) {
    if (!xs->initialized) {
        // Initial commit: the client is waiting for its first configure.
        xs->initialized = true;
        send_configure(xs);
        return;
    }
    if (xs->toplevel && xs->surface->has_buffer() && !xs->toplevel->mapped_) {
        xs->toplevel->mapped_ = true;
        ToplevelMap event{*xs->toplevel};
        xs->toplevel->map.emit(event);
    }
}

// ---- xdg_toplevel ----
// TODO: title/app_id/min/max/state requests are accepted no-ops — no window
// decorations or state machine yet, but real clients call them at startup so the
// slots must be non-null.
void toplevel_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void tl_set_parent(wl_client*, wl_resource*, wl_resource*) {}
void tl_set_title(wl_client*, wl_resource*, const char*) {}
void tl_set_app_id(wl_client*, wl_resource*, const char*) {}
void tl_show_window_menu(wl_client*, wl_resource*, wl_resource*, uint32_t, int32_t, int32_t) {}
void tl_move(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
void tl_resize(wl_client*, wl_resource*, wl_resource*, uint32_t, uint32_t) {}
void tl_set_max_size(wl_client*, wl_resource*, int32_t, int32_t) {}
void tl_set_min_size(wl_client*, wl_resource*, int32_t, int32_t) {}
void tl_set_maximized(wl_client*, wl_resource*) {}
void tl_unset_maximized(wl_client*, wl_resource*) {}
void tl_set_fullscreen(wl_client*, wl_resource*, wl_resource*) {}
void tl_unset_fullscreen(wl_client*, wl_resource*) {}
void tl_set_minimized(wl_client*, wl_resource*) {}
constexpr struct xdg_toplevel_interface toplevel_impl = {
    .destroy = toplevel_destroy_request,
    .set_parent = tl_set_parent,
    .set_title = tl_set_title,
    .set_app_id = tl_set_app_id,
    .show_window_menu = tl_show_window_menu,
    .move = tl_move,
    .resize = tl_resize,
    .set_max_size = tl_set_max_size,
    .set_min_size = tl_set_min_size,
    .set_maximized = tl_set_maximized,
    .unset_maximized = tl_unset_maximized,
    .set_fullscreen = tl_set_fullscreen,
    .unset_fullscreen = tl_unset_fullscreen,
    .set_minimized = tl_set_minimized,
};
void toplevel_resource_destroy(wl_resource* resource) {
    auto* tl = static_cast<ToplevelImpl*>(wl_resource_get_user_data(resource));
    ToplevelDestroy event{*tl};
    tl->destroy.emit(event);
    if (tl->owner != nullptr) {
        tl->owner->toplevel = nullptr; // stop the xdg_surface from touching us
    }
    delete tl;
}

// ---- xdg_positioner / xdg_popup ----
// TODO: inert but valid — popups get created and their xdg_surface still
// gets configured on commit, so clients don't crash or hang; full popup
// placement/grab is not implemented yet.
void generic_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void pos_set_size(wl_client*, wl_resource*, int32_t, int32_t) {}
void pos_set_anchor_rect(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void pos_set_u32(wl_client*, wl_resource*, uint32_t) {}
void pos_set_offset(wl_client*, wl_resource*, int32_t, int32_t) {}
constexpr struct xdg_positioner_interface positioner_impl = {
    .destroy = generic_destroy,
    .set_size = pos_set_size,
    .set_anchor_rect = pos_set_anchor_rect,
    .set_anchor = pos_set_u32,
    .set_gravity = pos_set_u32,
    .set_constraint_adjustment = pos_set_u32,
    .set_offset = pos_set_offset,
};
void popup_grab(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
constexpr struct xdg_popup_interface popup_impl = {
    .destroy = generic_destroy,
    .grab = popup_grab,
};

// ---- xdg_surface ----
XdgSurface* xdg_surface_of(wl_resource* resource) {
    return static_cast<XdgSurface*>(wl_resource_get_user_data(resource));
}
void xdg_surface_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void xdg_surface_get_toplevel(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* xs = xdg_surface_of(resource);
    auto* tl = new ToplevelImpl();
    tl->surf = xs->surface;
    tl->owner = xs;
    tl->resource = wl_resource_create(client, &xdg_toplevel_interface,
                                      wl_resource_get_version(resource), id);
    wl_resource_set_implementation(tl->resource, &toplevel_impl, tl, toplevel_resource_destroy);
    xs->toplevel = tl;

    NewToplevel event{*tl};
    xs->shell->new_toplevel.emit(event);
}
void xdg_surface_get_popup(wl_client* client, wl_resource* resource, uint32_t id, wl_resource*,
                           wl_resource*) {
    wl_resource* popup = wl_resource_create(client, &xdg_popup_interface,
                                            wl_resource_get_version(resource), id);
    if (popup != nullptr) {
        wl_resource_set_implementation(popup, &popup_impl, nullptr, nullptr);
    }
}
void xdg_surface_set_window_geometry(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void xdg_surface_ack_configure(wl_client*, wl_resource*, uint32_t) {}
constexpr struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy_request,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};
void xdg_surface_resource_destroy(wl_resource* resource) {
    auto* xs = xdg_surface_of(resource);
    if (xs->toplevel != nullptr) {
        // Our toplevel's resource outlives us; sever the back-pointer so its
        // destroy handler won't touch this freed XdgSurface.
        xs->toplevel->owner = nullptr;
    }
    delete xs;
}

// ---- xdg_wm_base ----
void wm_base_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void wm_base_get_xdg_surface(wl_client* client, wl_resource* wm_resource, uint32_t id,
                             wl_resource* surface_resource) {
    auto* shell = static_cast<XdgShell::Impl*>(wl_resource_get_user_data(wm_resource));
    auto* surface = static_cast<Surface*>(wl_resource_get_user_data(surface_resource));

    auto* xs = new XdgSurface{};
    xs->shell = shell;
    xs->surface = surface;
    xs->resource = wl_resource_create(client, &xdg_surface_interface,
                                      wl_resource_get_version(wm_resource), id);
    wl_resource_set_implementation(xs->resource, &xdg_surface_impl, xs,
                                   xdg_surface_resource_destroy);
    xs->commit_conn = surface->commit.connect([xs](SurfaceCommit&) { on_surface_commit(xs); });
}
void wm_base_create_positioner(wl_client* client, wl_resource* resource, uint32_t id) {
    wl_resource* pos = wl_resource_create(client, &xdg_positioner_interface,
                                          wl_resource_get_version(resource), id);
    if (pos != nullptr) {
        wl_resource_set_implementation(pos, &positioner_impl, nullptr, nullptr);
    }
}
void wm_base_pong(wl_client*, wl_resource*, uint32_t) {}
constexpr struct xdg_wm_base_interface wm_base_impl = {
    .destroy = wm_base_destroy_request,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = wm_base_pong,
};

void wm_base_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &xdg_wm_base_interface,
                                               static_cast<int>(version), id);
    wl_resource_set_implementation(resource, &wm_base_impl, data, nullptr);
}

} // namespace

XdgShell::XdgShell(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
XdgShell::~XdgShell() = default;
XdgShell::XdgShell(XdgShell&&) noexcept = default;
XdgShell& XdgShell::operator=(XdgShell&&) noexcept = default;

Result<XdgShell> XdgShell::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->global = wl_global_create(impl->display, &xdg_wm_base_interface, 1, impl.get(),
                                    wm_base_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(xdg_wm_base) failed");
    }
    return XdgShell{std::move(impl)};
}

Signal<NewToplevel>& XdgShell::new_toplevel() noexcept {
    return impl_->new_toplevel;
}

} // namespace luminaria
