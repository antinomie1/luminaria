module;

// Implements zwp_idle_inhibit_manager_v1 (version 1). One-way protocol: the
// only wire traffic is the client creating and destroying inhibitors, so all
// this file does is keep the live ones and tell the compositor when the answer
// to "may the screen blank?" changes.

#include <memory>

#include <algorithm>
#include <cstddef>
#include <typeinfo>
#include <vector>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "idle-inhibit-unstable-v1-protocol.h"

module luminaria;

namespace luminaria {

// Not in an anonymous namespace: IdleInhibitManager::Impl holds these, and a
// class with external linkage may not have a member of an internal-linkage type.
class InhibitorImpl;

struct IdleInhibitManager::Impl {
    wl_global* global = nullptr;
    std::vector<InhibitorImpl*> inhibitors;
    bool inhibited = false;

    Signal<NewIdleInhibitor> new_inhibitor;
    Signal<IdleInhibitChange> changed;

    /// Recompute from the live inhibitors and fire only on a transition — the
    /// compositor wires this straight to a blanking timer, and re-arming it on
    /// every unrelated inhibitor would keep the screen on forever.
    void recompute();

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

using Mgr = IdleInhibitManager::Impl;

// One zwp_idle_inhibitor_v1. Owned by its resource. The surface may die first,
// and an inhibitor without a surface is meaningless — so we destroy the
// resource from the surface's destroy signal rather than keeping a null one.
class InhibitorImpl final : public IdleInhibitor {
public:
    InhibitorImpl(Mgr* mgr, Surface& surface, wl_resource* resource)
        : mgr_(mgr), surface_(&surface), resource_(resource) {
        surface_gone_ = surface.destroy.connect([this](SurfaceDestroy&) {
            // Detach first: the resource destructor must not touch the Surface
            // that is halfway through being destroyed.
            surface_ = nullptr;
            wl_resource_destroy(resource_);
        });
    }

    Surface& surface() noexcept override { return *surface_; }
    [[nodiscard]] bool visible() const noexcept override { return visible_; }
    void set_visible(bool visible) override {
        if (visible_ == visible) {
            return;
        }
        visible_ = visible;
        mgr_->recompute();
    }

    Mgr* mgr_ = nullptr;
    Surface* surface_ = nullptr;
    wl_resource* resource_ = nullptr;
    bool visible_ = true;
    Signal<SurfaceDestroy>::Connection surface_gone_;
};

namespace {

void inhibitor_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

constexpr struct zwp_idle_inhibitor_v1_interface inhibitor_impl = {
    .destroy = inhibitor_destroy_request,
};

void inhibitor_resource_destroy(wl_resource* resource) {
    auto* inhibitor = static_cast<InhibitorImpl*>(wl_resource_get_user_data(resource));
    IdleInhibitorDestroy event{*inhibitor};
    inhibitor->destroy.emit(event);
    Mgr* mgr = inhibitor->mgr_;
    std::erase(mgr->inhibitors, inhibitor);
    delete inhibitor;
    mgr->recompute();
}

void manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void manager_create_inhibitor(wl_client* client, wl_resource* manager_resource, uint32_t id,
                              wl_resource* surface_resource) {
    auto* mgr = static_cast<Mgr*>(wl_resource_get_user_data(manager_resource));
    Surface* surface = surface_from_resource(surface_resource);
    wl_resource* resource =
        wl_resource_create(client, &zwp_idle_inhibitor_v1_interface,
                           wl_resource_get_version(manager_resource), static_cast<int>(id));
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    if (surface == nullptr) {
        // Not a wl_surface we know. The protocol has no error for it, so the
        // object is created and simply inhibits nothing.
        wl_resource_set_implementation(resource, &inhibitor_impl, nullptr, nullptr);
        return;
    }
    auto* inhibitor = new InhibitorImpl(mgr, *surface, resource);
    wl_resource_set_implementation(resource, &inhibitor_impl, inhibitor,
                                   inhibitor_resource_destroy);
    mgr->inhibitors.push_back(inhibitor);

    NewIdleInhibitor event{*inhibitor};
    mgr->new_inhibitor.emit(event);
    // …after the signal, so a handler that immediately hid the surface is
    // reflected in the first `changed` the compositor sees.
    mgr->recompute();
}

constexpr struct zwp_idle_inhibit_manager_v1_interface manager_impl = {
    .destroy = manager_destroy_request,
    .create_inhibitor = manager_create_inhibitor,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zwp_idle_inhibit_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

} // namespace

void IdleInhibitManager::Impl::recompute() {
    const bool now = std::ranges::any_of(
        inhibitors, [](const InhibitorImpl* i) { return i->visible_; });
    if (now == inhibited) {
        return;
    }
    inhibited = now;
    IdleInhibitChange event{now};
    changed.emit(event);
}

IdleInhibitManager::IdleInhibitManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
IdleInhibitManager::~IdleInhibitManager() = default;
IdleInhibitManager::IdleInhibitManager(IdleInhibitManager&&) noexcept = default;
IdleInhibitManager& IdleInhibitManager::operator=(IdleInhibitManager&&) noexcept = default;

Result<IdleInhibitManager> IdleInhibitManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->global = wl_global_create(display.c_ptr(), &zwp_idle_inhibit_manager_v1_interface, 1,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(zwp_idle_inhibit_manager_v1) failed");
    }
    return IdleInhibitManager{std::move(impl)};
}

Signal<NewIdleInhibitor>& IdleInhibitManager::new_inhibitor() noexcept {
    return impl_->new_inhibitor;
}
Signal<IdleInhibitChange>& IdleInhibitManager::changed() noexcept {
    return impl_->changed;
}
bool IdleInhibitManager::inhibited() const noexcept {
    return impl_->inhibited;
}
std::size_t IdleInhibitManager::inhibitor_count() const noexcept {
    return impl_->inhibitors.size();
}

} // namespace luminaria
