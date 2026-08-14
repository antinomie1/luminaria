// luminaria/fractional_scale.cppm — wp_fractional_scale_v1.
//
// `wl_output.scale` is an integer, so a 150% display can only be told to clients
// as "1" (too small) or "2" (too big, then downscaled — blurry). This protocol
// says the real number, in 120ths: 180 is 1.5x.
//
// The client then renders at that density and uses wp_viewporter to declare the
// logical size of the buffer it produced, which is why the two travel together.

module;

#include <memory>

#include <algorithm>
#include <cstdint>
#include <vector>
#include <wayland-server-core.h>
#include "fractional-scale-v1-protocol.h"

export module luminaria:fractional_scale;

import :compositor;
import :display;
import :expected;
import :signal;

export namespace luminaria {

class Display;
class Surface;

class FractionalScaleManager {
public:
    [[nodiscard]] static Result<FractionalScaleManager> create(Display& display);

    ~FractionalScaleManager();
    FractionalScaleManager(FractionalScaleManager&&) noexcept;
    FractionalScaleManager& operator=(FractionalScaleManager&&) noexcept;
    FractionalScaleManager(const FractionalScaleManager&) = delete;
    FractionalScaleManager& operator=(const FractionalScaleManager&) = delete;

    /// Tell a surface the scale of the output it is showing on. `scale_120ths`
    /// is the protocol's unit (120 = 1x, 180 = 1.5x, 240 = 2x); values below 1
    /// are ignored. Call it when a surface lands on an output, and again when
    /// that output's scale changes. Surfaces with no wp_fractional_scale_v1
    /// object are silently skipped.
    void set_scale(Surface& surface, int scale_120ths);

    /// The same thing from a floating-point scale, e.g. 1.5.
    void set_scale(Surface& surface, double scale) {
        set_scale(surface, static_cast<int>(scale * 120.0 + 0.5));
    }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit FractionalScaleManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

namespace {
/// One wp_fractional_scale_v1, owned by its resource. `surface` is nulled by the
/// destroy subscription, so a dead surface can never be sent an event.
struct ScaleObject {
    Surface* surface = nullptr;
    wl_resource* resource = nullptr;
    int last_sent = 0;
    Signal<SurfaceDestroy>::Connection destroy_conn;
};
} // namespace

struct FractionalScaleManager::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    std::vector<ScaleObject*> objects; // not owned; each lives on its resource

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

namespace {

using FsMgr = FractionalScaleManager::Impl;

struct ScaleOwner {
    FsMgr* mgr = nullptr;
    ScaleObject* object = nullptr;
};

ScaleOwner* owner_of(wl_resource* r) { return static_cast<ScaleOwner*>(wl_resource_get_user_data(r)); }

void scale_destroy_request(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

constexpr struct wp_fractional_scale_v1_interface scale_impl = {
    .destroy = scale_destroy_request,
};

void scale_resource_destroy(wl_resource* resource) {
    ScaleOwner* owner = owner_of(resource);
    std::erase(owner->mgr->objects, owner->object);
    delete owner->object;
    delete owner;
}

void manager_destroy_request(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void manager_get_scale(wl_client* client, wl_resource* resource, uint32_t id,
                       wl_resource* surface_resource) {
    auto* mgr = static_cast<FsMgr*>(wl_resource_get_user_data(resource));
    Surface* surface = surface_from_resource(surface_resource);
    if (surface != nullptr) {
        for (const ScaleObject* existing : mgr->objects) {
            if (existing->surface == surface) {
                wl_resource_post_error(resource,
                                       WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
                                       "this surface already has a fractional scale object");
                return;
            }
        }
    }
    wl_resource* scale_resource = wl_resource_create(client, &wp_fractional_scale_v1_interface,
                                                     wl_resource_get_version(resource), id);
    if (scale_resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* object = new ScaleObject{};
    object->surface = surface;
    object->resource = scale_resource;
    if (surface != nullptr) {
        object->destroy_conn =
            surface->destroy.connect([object](SurfaceDestroy&) { object->surface = nullptr; });
    }
    mgr->objects.push_back(object);
    wl_resource_set_implementation(scale_resource, &scale_impl, new ScaleOwner{mgr, object},
                                   scale_resource_destroy);
}

constexpr struct wp_fractional_scale_manager_v1_interface manager_impl = {
    .destroy = manager_destroy_request,
    .get_fractional_scale = manager_get_scale,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wp_fractional_scale_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

} // namespace

FractionalScaleManager::FractionalScaleManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
FractionalScaleManager::~FractionalScaleManager() = default;
FractionalScaleManager::FractionalScaleManager(FractionalScaleManager&&) noexcept = default;
FractionalScaleManager& FractionalScaleManager::operator=(FractionalScaleManager&&) noexcept =
    default;

Result<FractionalScaleManager> FractionalScaleManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->global = wl_global_create(impl->display, &wp_fractional_scale_manager_v1_interface, 1,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wp_fractional_scale_manager_v1) failed");
    }
    return FractionalScaleManager{std::move(impl)};
}

void FractionalScaleManager::set_scale(Surface& surface, int scale_120ths) {
    if (scale_120ths < 1) {
        return;
    }
    for (ScaleObject* object : impl_->objects) {
        if (object->surface != &surface || object->last_sent == scale_120ths) {
            continue;
        }
        object->last_sent = scale_120ths;
        wp_fractional_scale_v1_send_preferred_scale(object->resource,
                                                    static_cast<uint32_t>(scale_120ths));
    }
}

} // namespace luminaria
