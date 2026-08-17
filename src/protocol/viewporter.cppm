// luminaria/viewporter.cppm — wp_viewporter: crop and scale a surface's buffer.
//
// A client attaches one buffer and says "show this sub-rectangle of it, at this
// size". Video players use it to letterbox without re-encoding; every client
// doing fractional scaling uses it to render at, say, 1.5x into an integer
// buffer and have the compositor stretch it to the right logical size.
//
// The state lands on the Surface (`viewport_src_*`, `surface_width/height`); the
// renderer reads it from there, so nothing else has to know this global exists.

module;


#include <cstdint>
#include <wayland-server-core.h>
#include "viewporter-protocol.h"

export module luminaria:viewporter;

import std;

import :compositor;
import :display;
import :expected;
import :protocol_helper;
import :signal;

export namespace luminaria {

class Display;

class Viewporter {
public:
    [[nodiscard]] static Result<Viewporter> create(Display& display);

    ~Viewporter();
    Viewporter(Viewporter&&) noexcept;
    Viewporter& operator=(Viewporter&&) noexcept;
    Viewporter(const Viewporter&) = delete;
    Viewporter& operator=(const Viewporter&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit Viewporter(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

namespace {

/// One wp_viewport. Owned by its resource; the Surface may die first, which is
/// why the viewport watches it rather than caching blindly.
struct Viewport : SurfaceTracker {
    using SurfaceTracker::SurfaceTracker;

    ~Viewport() override {
        // Destroying the viewport puts the surface back to "the whole buffer, at
        // its own size" — otherwise it would keep a crop nobody can change.
        if (surface != nullptr) {
            surface->set_pending_viewport_source(0, 0, -1, -1);
            surface->set_pending_viewport_destination(-1, -1);
        }
    }
};

double from_fixed(wl_fixed_t v) { return wl_fixed_to_double(v); }

void viewport_set_source(wl_client*, wl_resource* resource, wl_fixed_t x, wl_fixed_t y,
                         wl_fixed_t width, wl_fixed_t height) {
    auto* vp = static_cast<Viewport*>(wl_resource_get_user_data(resource));
    const double dx = from_fixed(x), dy = from_fixed(y);
    const double dw = from_fixed(width), dh = from_fixed(height);
    // -1,-1,-1,-1 is the protocol's "unset"; anything else must be a real rect.
    const bool unset = dx == -1.0 && dy == -1.0 && dw == -1.0 && dh == -1.0;
    if (!unset && (dx < 0 || dy < 0 || dw <= 0 || dh <= 0)) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
                               "source rectangle must be non-negative and non-empty");
        return;
    }
    if (vp->surface == nullptr) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
                               "the wl_surface is gone");
        return;
    }
    vp->surface->set_pending_viewport_source(dx, dy, unset ? -1 : dw, unset ? -1 : dh);
}

void viewport_set_destination(wl_client*, wl_resource* resource, int32_t width, int32_t height) {
    auto* vp = static_cast<Viewport*>(wl_resource_get_user_data(resource));
    const bool unset = width == -1 && height == -1;
    if (!unset && (width <= 0 || height <= 0)) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
                               "destination size must be positive");
        return;
    }
    if (vp->surface == nullptr) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
                               "the wl_surface is gone");
        return;
    }
    vp->surface->set_pending_viewport_destination(unset ? -1 : width, unset ? -1 : height);
}

constexpr struct wp_viewport_interface viewport_impl = {
    .destroy = resource_destroy_request,
    .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};

void viewporter_get_viewport(wl_client* client, wl_resource* resource, uint32_t id,
                             wl_resource* surface_resource) {
    Surface* surface = surface_from_resource(surface_resource);
    auto vp = std::make_unique<Viewport>(surface);
    create_user_resource<Viewport, &wp_viewport_interface, &viewport_impl>(
        client, wl_resource_get_version(resource), id, std::move(vp));
}

constexpr struct wp_viewporter_interface viewporter_impl = {
    .destroy = resource_destroy_request,
    .get_viewport = viewporter_get_viewport,
};

} // namespace

struct Viewporter::Impl {
    WlGlobal global;
};

Viewporter::Viewporter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Viewporter::~Viewporter() = default;
Viewporter::Viewporter(Viewporter&&) noexcept = default;
Viewporter& Viewporter::operator=(Viewporter&&) noexcept = default;

Result<Viewporter> Viewporter::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    auto global = create_wl_global<&wp_viewporter_interface,
                                   default_bind<&wp_viewporter_interface,
                                                &viewporter_impl>>(display, 1, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return Viewporter{std::move(impl)};
}

} // namespace luminaria
