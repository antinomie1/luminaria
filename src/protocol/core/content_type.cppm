// luminaria/content_type.cppm — wp_content_type_manager_v1: "what I am showing
// is a video".
//
// One word from the client that the compositor cannot work out for itself. A
// video player wants its frames on screen unmangled and the display left on; a
// game wants latency; a photo viewer wants neither. The hint lands on the
// Surface (`Surface::content_type()`) and is read wherever a policy needs it —
// picking a refresh rate, preferring direct scanout, holding off the blanker.
//
// There is no wire traffic back, so this file is only the plumbing: one global,
// one object per surface, and double-buffered state like everything else on a
// wl_surface.

module;

#include <cstdint>

#include <wayland-server-core.h>
#include "content-type-v1-protocol.h"

export module luminaria:content_type;

import std;

import :compositor;
import :display;
import :expected;
import :protocol_helper;
import :signal;

export namespace luminaria {

class Display;

/// What a client says it is showing. Matches `wp_content_type_v1.type`, which
/// is also what `Surface::content_type()` reports.
enum class ContentType : std::uint32_t {
    none = 0,
    photo = 1,
    video = 2,
    game = 3,
};

/// The wp_content_type_manager_v1 global (version 1). Move-only; pointer-stable
/// state so the libwayland global can hold a pointer to it.
class ContentTypeManager {
public:
    [[nodiscard]] static Result<ContentTypeManager> create(Display& display);

    ~ContentTypeManager();
    ContentTypeManager(ContentTypeManager&&) noexcept;
    ContentTypeManager& operator=(ContentTypeManager&&) noexcept;
    ContentTypeManager(const ContentTypeManager&) = delete;
    ContentTypeManager& operator=(const ContentTypeManager&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit ContentTypeManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Implements wp_content_type_manager_v1 (version 1). Shaped exactly like
// tearing-control, for the same reason: a per-surface hint with no answer.

namespace luminaria {

namespace {

struct ContentTypeObject : SurfaceTracker {
    using SurfaceTracker::SurfaceTracker;

    ~ContentTypeObject() override {
        if (surface != nullptr) {
            surface->set_pending_content_type(WP_CONTENT_TYPE_V1_TYPE_NONE);
        }
    }
};

void ct_set_content_type(wl_client*, wl_resource* resource, uint32_t type) {
    auto* object = static_cast<ContentTypeObject*>(wl_resource_get_user_data(resource));
    if (object->surface != nullptr) {
        object->surface->set_pending_content_type(type);
    }
}

constexpr struct wp_content_type_v1_interface ct_impl = {
    .destroy = resource_destroy_request,
    .set_content_type = ct_set_content_type,
};

void manager_get_surface_content_type(wl_client* client, wl_resource* manager, uint32_t id,
                                      wl_resource* surface_resource) {
    Surface* surface = surface_from_resource(surface_resource);
    auto object = std::make_unique<ContentTypeObject>(surface);
    create_user_resource<ContentTypeObject, &wp_content_type_v1_interface, &ct_impl>(
        client, wl_resource_get_version(manager), id, std::move(object), manager);
}

constexpr struct wp_content_type_manager_v1_interface manager_impl = {
    .destroy = resource_destroy_request,
    .get_surface_content_type = manager_get_surface_content_type,
};

} // namespace

struct ContentTypeManager::Impl {
    WlGlobal global;
};

ContentTypeManager::ContentTypeManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
ContentTypeManager::~ContentTypeManager() = default;
ContentTypeManager::ContentTypeManager(ContentTypeManager&&) noexcept = default;
ContentTypeManager& ContentTypeManager::operator=(ContentTypeManager&&) noexcept = default;

Result<ContentTypeManager> ContentTypeManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    auto global = create_wl_global<&wp_content_type_manager_v1_interface,
                                   default_bind<&wp_content_type_manager_v1_interface,
                                                &manager_impl>>(display, 1, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return ContentTypeManager{std::move(impl)};
}

} // namespace luminaria
