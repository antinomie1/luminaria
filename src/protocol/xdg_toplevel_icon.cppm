// luminaria/xdg_toplevel_icon.cppm — client-supplied xdg toplevel icons.
module;

#include <wayland-server-core.h>
#include "xdg-toplevel-icon-v1-protocol.h"

export module luminaria:xdg_toplevel_icon;

import std;

import :client_buffer;
import :display;
import :expected;
import :protocol_helper;
import :signal;
import :xdg_shell;

export namespace luminaria {

struct IconPixels {
    int width = 0;
    int height = 0;
    int scale = 1;
    std::vector<std::uint8_t> rgba;
};

struct ToplevelIcon {
    std::string name;
    std::vector<IconPixels> images;
};

struct ToplevelIconChange {
    Toplevel& toplevel;
};

class XdgToplevelIconManager {
public:
    [[nodiscard]] static Result<XdgToplevelIconManager> create(Display& display);
    ~XdgToplevelIconManager();
    XdgToplevelIconManager(XdgToplevelIconManager&&) noexcept;
    XdgToplevelIconManager& operator=(XdgToplevelIconManager&&) noexcept;
    XdgToplevelIconManager(const XdgToplevelIconManager&) = delete;
    XdgToplevelIconManager& operator=(const XdgToplevelIconManager&) = delete;

    [[nodiscard]] const ToplevelIcon* icon_for(const Toplevel& toplevel) const noexcept;
    [[nodiscard]] Signal<ToplevelIconChange>& changed() noexcept;
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit XdgToplevelIconManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct XdgToplevelIconManager::Impl {
    WlGlobal global;
    std::map<const Toplevel*, ToplevelIcon> icons;
    std::map<const Toplevel*, Signal<ToplevelDestroy>::Connection> destroy_connections;
    Signal<ToplevelIconChange> changed;
};

namespace {
using Manager = XdgToplevelIconManager::Impl;
struct PendingImage { BufferContents contents; int scale = 1; };
struct Icon {
    Manager* manager = nullptr;
    std::string name;
    std::vector<PendingImage> images;
    bool immutable = false;
};

Icon* icon_of(wl_resource* resource) {
    return static_cast<Icon*>(wl_resource_get_user_data(resource));
}

void icon_set_name(wl_client*, wl_resource* resource, const char* name) {
    Icon* icon = icon_of(resource);
    if (icon->immutable) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE,
                               "an assigned icon is immutable");
        return;
    }
    icon->name = name != nullptr ? name : "";
}

void icon_add_buffer(wl_client*, wl_resource* resource, wl_resource* buffer, int32_t scale) {
    Icon* icon = icon_of(resource);
    if (icon->immutable) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE,
                               "an assigned icon is immutable");
        return;
    }
    BufferContents contents = retain_client_buffer(buffer);
    if (!contents || contents.width() <= 0 || contents.width() != contents.height() || scale <= 0) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER,
                               "icon buffers must be square wl_shm buffers");
        return;
    }
    std::erase_if(icon->images, [&](const PendingImage& image) {
        return image.contents.width() == contents.width() && image.scale == scale;
    });
    icon->images.push_back({std::move(contents), scale});
}

constexpr struct xdg_toplevel_icon_v1_interface icon_impl = {
    .destroy = resource_destroy_request,
    .set_name = icon_set_name,
    .add_buffer = icon_add_buffer,
};

void manager_create_icon(wl_client* client, wl_resource* resource, uint32_t id) {
    wl_resource* created = wl_resource_create(client, &xdg_toplevel_icon_v1_interface, 1, id);
    if (created == nullptr) { wl_client_post_no_memory(client); return; }
    auto icon = std::make_unique<Icon>();
    icon->manager = static_cast<Manager*>(wl_resource_get_user_data(resource));
    wl_resource_set_implementation(created, &icon_impl, icon.release(), [](wl_resource* r) {
        delete icon_of(r);
    });
}

void manager_set_icon(wl_client*, wl_resource* resource, wl_resource* toplevel_resource,
                      wl_resource* icon_resource) {
    auto* manager = static_cast<Manager*>(wl_resource_get_user_data(resource));
    Toplevel* toplevel = toplevel_from_resource(toplevel_resource);
    if (toplevel == nullptr) return;
    ToplevelIcon applied;
    if (icon_resource != nullptr) {
        Icon* icon = icon_of(icon_resource);
        icon->immutable = true;
        applied.name = icon->name;
        for (const PendingImage& pending : icon->images) {
            IconPixels image;
            image.scale = pending.scale;
            if (pending.contents.rgba(image.rgba, image.width, image.height)) {
                applied.images.push_back(std::move(image));
            }
        }
    }
    manager->icons[toplevel] = std::move(applied);
    if (!manager->destroy_connections.contains(toplevel)) {
        manager->destroy_connections.emplace(
            toplevel, toplevel->destroy.connect([manager](ToplevelDestroy& event) {
                manager->icons.erase(&event.toplevel);
                manager->destroy_connections.erase(&event.toplevel);
            }));
    }
    ToplevelIconChange event{*toplevel};
    manager->changed.emit(event);
}

constexpr struct xdg_toplevel_icon_manager_v1_interface manager_impl = {
    .destroy = resource_destroy_request,
    .create_icon = manager_create_icon,
    .set_icon = manager_set_icon,
};

void bind_manager(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(
        client, &xdg_toplevel_icon_manager_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
    xdg_toplevel_icon_manager_v1_send_icon_size(resource, 16);
    xdg_toplevel_icon_manager_v1_send_icon_size(resource, 32);
    xdg_toplevel_icon_manager_v1_send_done(resource);
}
} // namespace

XdgToplevelIconManager::XdgToplevelIconManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
XdgToplevelIconManager::~XdgToplevelIconManager() = default;
XdgToplevelIconManager::XdgToplevelIconManager(XdgToplevelIconManager&&) noexcept = default;
XdgToplevelIconManager& XdgToplevelIconManager::operator=(XdgToplevelIconManager&&) noexcept = default;

Result<XdgToplevelIconManager> XdgToplevelIconManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    auto global = create_wl_global<&xdg_toplevel_icon_manager_v1_interface, bind_manager>(
        display, 1, impl.get());
    if (!global) return fail(std::move(global.error().message));
    impl->global = std::move(*global);
    return XdgToplevelIconManager{std::move(impl)};
}

const ToplevelIcon* XdgToplevelIconManager::icon_for(const Toplevel& toplevel) const noexcept {
    const auto found = impl_->icons.find(&toplevel);
    return found != impl_->icons.end() ? &found->second : nullptr;
}

Signal<ToplevelIconChange>& XdgToplevelIconManager::changed() noexcept { return impl_->changed; }

} // namespace luminaria
