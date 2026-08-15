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
#include <memory>

#include <utility>
#include <wayland-server-core.h>
#include "content-type-v1-protocol.h"

export module luminaria:content_type;

import :compositor;
import :display;
import :expected;
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

// One wp_content_type_v1. Turns inert if its surface dies first.
struct ContentTypeObject {
    Surface* surface = nullptr;
    Signal<SurfaceDestroy>::Connection on_surface_destroy;
};

void ct_set_content_type(wl_client*, wl_resource* resource, uint32_t type) {
    auto* object = static_cast<ContentTypeObject*>(wl_resource_get_user_data(resource));
    if (object->surface != nullptr) {
        object->surface->set_pending_content_type(type);
    }
}

void ct_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void ct_resource_destroy(wl_resource* resource) {
    auto* object = static_cast<ContentTypeObject*>(wl_resource_get_user_data(resource));
    // "The content type is reset to none" — but not until the next commit, like
    // every other piece of surface state.
    if (object->surface != nullptr) {
        object->surface->set_pending_content_type(WP_CONTENT_TYPE_V1_TYPE_NONE);
    }
    delete object;
}

constexpr struct wp_content_type_v1_interface ct_impl = {
    .destroy = ct_destroy_request,
    .set_content_type = ct_set_content_type,
};

void manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

// No `already_constructed` error, for the reason tearing_control.cppm gives:
// catching a duplicate needs a surface->object map that outlives the surface,
// and a second object simply wins instead.
void manager_get_surface_content_type(wl_client* client, wl_resource* manager, uint32_t id,
                                      wl_resource* surface_resource) {
    Surface* surface = surface_from_resource(surface_resource);
    wl_resource* resource = wl_resource_create(client, &wp_content_type_v1_interface,
                                               wl_resource_get_version(manager),
                                               static_cast<int>(id));
    if (resource == nullptr) {
        wl_resource_post_no_memory(manager);
        return;
    }
    auto* object = new ContentTypeObject{surface, {}};
    if (surface != nullptr) {
        object->on_surface_destroy =
            surface->destroy.connect([object](SurfaceDestroy&) { object->surface = nullptr; });
    }
    wl_resource_set_implementation(resource, &ct_impl, object, ct_resource_destroy);
}

constexpr struct wp_content_type_manager_v1_interface manager_impl = {
    .destroy = manager_destroy_request,
    .get_surface_content_type = manager_get_surface_content_type,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wp_content_type_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

} // namespace

struct ContentTypeManager::Impl {
    wl_global* global = nullptr;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

ContentTypeManager::ContentTypeManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
ContentTypeManager::~ContentTypeManager() = default;
ContentTypeManager::ContentTypeManager(ContentTypeManager&&) noexcept = default;
ContentTypeManager& ContentTypeManager::operator=(ContentTypeManager&&) noexcept = default;

Result<ContentTypeManager> ContentTypeManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->global = wl_global_create(display.c_ptr(), &wp_content_type_manager_v1_interface, 1,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wp_content_type_manager_v1) failed");
    }
    return ContentTypeManager{std::move(impl)};
}

} // namespace luminaria
