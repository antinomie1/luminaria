// luminaria/subcompositor.cppm — the wl_subcompositor global (subsurfaces).
//
// A subsurface is a wl_surface positioned relative to a parent wl_surface and
// stacked below or above it. Toolkits use them for video layers, client-side
// decorations, and popups drawn inside a window. The tree itself lives on
// `Surface` (see compositor.cppm: subsurface_parent / surface_tree /
// surface_at); this global is just the protocol glue that builds it.
//
// Sync mode (the protocol default) is implemented: a synced subsurface's commit
// is cached and applied atomically when its parent commits.

module;


#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

export module luminaria:subcompositor;

import std;

import :compositor;
import :display;
import :expected;
import :signal;

export namespace luminaria {

class Display;

/// The wl_subcompositor global (protocol version 1). Move-only; pointer-stable
/// state so the libwayland global can hold a pointer to it.
class Subcompositor {
public:
    [[nodiscard]] static Result<Subcompositor> create(Display& display);

    ~Subcompositor();
    Subcompositor(Subcompositor&&) noexcept;
    Subcompositor& operator=(Subcompositor&&) noexcept;
    Subcompositor(const Subcompositor&) = delete;
    Subcompositor& operator=(const Subcompositor&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit Subcompositor(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct Subcompositor::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

namespace {

// Owned by its wl_subsurface resource. `surface` is nulled if the wl_surface
// dies first, so teardown in either resource order is safe.
struct SubsurfaceGlue {
    Surface* surface = nullptr;
    Signal<SurfaceDestroy>::Connection on_surface_destroy;
};

SubsurfaceGlue* glue_of(wl_resource* resource) {
    return static_cast<SubsurfaceGlue*>(wl_resource_get_user_data(resource));
}

Surface* surface_arg(wl_resource* resource) {
    return resource == nullptr ? nullptr : static_cast<Surface*>(wl_resource_get_user_data(resource));
}

void subsurface_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void subsurface_set_position(wl_client*, wl_resource* resource, int32_t x, int32_t y) {
    if (Surface* s = glue_of(resource)->surface; s != nullptr) {
        s->sub_set_position(x, y);
    }
}
void subsurface_place_above(wl_client*, wl_resource* resource, wl_resource* sibling_resource) {
    Surface* s = glue_of(resource)->surface;
    Surface* sibling = surface_arg(sibling_resource);
    if (s == nullptr || sibling == nullptr) {
        return;
    }
    if (!s->sub_place(*sibling, true)) {
        wl_resource_post_error(resource, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                               "place_above: not a sibling or the parent");
    }
}
void subsurface_place_below(wl_client*, wl_resource* resource, wl_resource* sibling_resource) {
    Surface* s = glue_of(resource)->surface;
    Surface* sibling = surface_arg(sibling_resource);
    if (s == nullptr || sibling == nullptr) {
        return;
    }
    if (!s->sub_place(*sibling, false)) {
        wl_resource_post_error(resource, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                               "place_below: not a sibling or the parent");
    }
}
void subsurface_set_sync(wl_client*, wl_resource* resource) {
    if (Surface* s = glue_of(resource)->surface; s != nullptr) {
        s->sub_set_sync(true);
    }
}
void subsurface_set_desync(wl_client*, wl_resource* resource) {
    if (Surface* s = glue_of(resource)->surface; s != nullptr) {
        s->sub_set_sync(false);
    }
}
constexpr struct wl_subsurface_interface subsurface_impl = {
    .destroy = subsurface_destroy_request,
    .set_position = subsurface_set_position,
    .place_above = subsurface_place_above,
    .place_below = subsurface_place_below,
    .set_sync = subsurface_set_sync,
    .set_desync = subsurface_set_desync,
};

void subsurface_resource_destroy(wl_resource* resource) {
    auto* glue = glue_of(resource);
    if (glue->surface != nullptr) {
        glue->surface->sub_detach(); // the surface goes back to being standalone
    }
    delete glue;
}

// ---- wl_subcompositor ----
void subcompositor_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void subcompositor_get_subsurface(wl_client* client, wl_resource* resource, uint32_t id,
                                  wl_resource* surface_resource, wl_resource* parent_resource) {
    Surface* surface = surface_arg(surface_resource);
    Surface* parent = surface_arg(parent_resource);
    if (surface == nullptr || parent == nullptr) {
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "get_subsurface: null surface");
        return;
    }
    if (surface->subsurface_parent() != nullptr) {
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "get_subsurface: surface already has a parent");
        return;
    }
    if (!surface->sub_attach(*parent)) {
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
                               "get_subsurface: parent is the surface or its descendant");
        return;
    }
    wl_resource* sub = wl_resource_create(client, &wl_subsurface_interface,
                                          wl_resource_get_version(resource), id);
    if (sub == nullptr) {
        surface->sub_detach();
        wl_client_post_no_memory(client);
        return;
    }
    auto* glue = new SubsurfaceGlue{};
    glue->surface = surface;
    // If the wl_surface dies first, forget it: the wl_subsurface may outlive it.
    glue->on_surface_destroy =
        surface->destroy.connect([glue](SurfaceDestroy&) { glue->surface = nullptr; });
    wl_resource_set_implementation(sub, &subsurface_impl, glue, subsurface_resource_destroy);
}

constexpr struct wl_subcompositor_interface subcompositor_impl = {
    .destroy = subcompositor_destroy_request,
    .get_subsurface = subcompositor_get_subsurface,
};

void subcompositor_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_subcompositor_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &subcompositor_impl, data, nullptr);
}

} // namespace

Subcompositor::Subcompositor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Subcompositor::~Subcompositor() = default;
Subcompositor::Subcompositor(Subcompositor&&) noexcept = default;
Subcompositor& Subcompositor::operator=(Subcompositor&&) noexcept = default;

Result<Subcompositor> Subcompositor::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->global = wl_global_create(impl->display, &wl_subcompositor_interface, 1, impl.get(),
                                    subcompositor_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wl_subcompositor) failed");
    }
    return Subcompositor{std::move(impl)};
}

} // namespace luminaria
