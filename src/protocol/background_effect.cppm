// luminaria/background_effect.cppm — ext-background-effect-v1: client-declared
// background blur regions.
//
// The protocol deliberately does not prescribe how a compositor blurs. This
// partition only turns its double-buffered wl_region into Surface::blur_region;
// the shell/renderer decides whether that hint becomes x-ray blur, lower-window
// sampling, or no effect at all.

module;

#include <memory>
#include <vector>

#include <algorithm>
#include <utility>
#include <wayland-server-core.h>
#include "ext-background-effect-v1-protocol.h"

export module luminaria:background_effect;

import :compositor;
import :display;
import :expected;
import :region;
import :signal;

export namespace luminaria {

class Display;

/// The ext-background-effect-v1 global. Creating it advertises that the
/// compositor accepts blur-region hints; surfaces retain the region even when
/// a particular output chooses not to render blur.
class BackgroundEffectManager {
public:
    [[nodiscard]] static Result<BackgroundEffectManager> create(Display& display);

    ~BackgroundEffectManager();
    BackgroundEffectManager(BackgroundEffectManager&&) noexcept;
    BackgroundEffectManager& operator=(BackgroundEffectManager&&) noexcept;
    BackgroundEffectManager(const BackgroundEffectManager&) = delete;
    BackgroundEffectManager& operator=(const BackgroundEffectManager&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit BackgroundEffectManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation

namespace luminaria {

struct BackgroundEffectManager::Impl {
    wl_global* global = nullptr;
    std::vector<SurfaceId> active;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

namespace {

struct BackgroundEffectObject {
    Surface* surface = nullptr;
    SurfaceId id;
    BackgroundEffectManager::Impl* owner = nullptr;
    Signal<SurfaceDestroy>::Connection on_surface_destroy;
};

void effect_set_blur_region(wl_client*, wl_resource* resource, wl_resource* region_resource) {
    auto* effect = static_cast<BackgroundEffectObject*>(wl_resource_get_user_data(resource));
    if (effect->surface == nullptr) {
        wl_resource_post_error(resource, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
                               "the associated wl_surface was destroyed");
        return;
    }
    effect->surface->set_pending_blur_region(region_from_resource(region_resource));
}

void effect_destroy_request(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void effect_resource_destroy(wl_resource* resource) {
    auto* effect = static_cast<BackgroundEffectObject*>(wl_resource_get_user_data(resource));
    if (effect->surface != nullptr) {
        // The protocol removes the effect on the surface's next commit.
        effect->surface->set_pending_blur_region(nullptr);
    }
    if (effect->owner != nullptr) {
        std::erase(effect->owner->active, effect->id);
    }
    delete effect;
}

constexpr struct ext_background_effect_surface_v1_interface effect_impl = {
    .destroy = effect_destroy_request,
    .set_blur_region = effect_set_blur_region,
};

void manager_destroy_request(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void manager_get_background_effect(wl_client* client, wl_resource* manager, std::uint32_t id,
                                   wl_resource* surface_resource) {
    auto* owner = static_cast<BackgroundEffectManager::Impl*>(wl_resource_get_user_data(manager));
    Surface* surface = surface_from_resource(surface_resource);
    if (surface != nullptr && std::find(owner->active.begin(), owner->active.end(), surface->id()) !=
                                  owner->active.end()) {
        wl_resource_post_error(manager,
                               EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
                               "wl_surface already has an ext_background_effect_surface_v1");
        return;
    }
    wl_resource* resource = wl_resource_create(client, &ext_background_effect_surface_v1_interface,
                                               wl_resource_get_version(manager),
                                               static_cast<int>(id));
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* effect = new BackgroundEffectObject{surface, surface != nullptr ? surface->id() : SurfaceId{},
                                              owner, {}};
    if (surface != nullptr) {
        owner->active.push_back(effect->id);
        effect->on_surface_destroy = surface->destroy.connect([effect](SurfaceDestroy&) {
            effect->surface = nullptr;
        });
    }
    wl_resource_set_implementation(resource, &effect_impl, effect, effect_resource_destroy);
}

constexpr struct ext_background_effect_manager_v1_interface manager_impl = {
    .destroy = manager_destroy_request,
    .get_background_effect = manager_get_background_effect,
};

void manager_bind(wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &ext_background_effect_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
    ext_background_effect_manager_v1_send_capabilities(
        resource, EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
}

} // namespace

BackgroundEffectManager::BackgroundEffectManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
BackgroundEffectManager::~BackgroundEffectManager() = default;
BackgroundEffectManager::BackgroundEffectManager(BackgroundEffectManager&&) noexcept = default;
BackgroundEffectManager& BackgroundEffectManager::operator=(BackgroundEffectManager&&) noexcept = default;

Result<BackgroundEffectManager> BackgroundEffectManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->global = wl_global_create(display.c_ptr(), &ext_background_effect_manager_v1_interface, 1,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(ext_background_effect_manager_v1) failed");
    }
    return BackgroundEffectManager{std::move(impl)};
}

} // namespace luminaria
