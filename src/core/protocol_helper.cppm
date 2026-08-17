// luminaria/core/protocol_helper.cppm — reusable C++23 templates & RAII utilities for Wayland protocols.
//
// Eliminates repetitive protocol boilerplate (global creation, bind callbacks,
// resource lifetime & destroy handlers, surface tracking) without adding any runtime overhead.

module;

#include "detail/wayland_fwd.h"
#include <cstdint>
#include <format>
#include <memory>
#include <typeinfo>
#include <utility>

#include <wayland-server-core.h>

export module luminaria:protocol_helper;

import std;

import :compositor;
import :display;
import :expected;
import :handle;
import :signal;

export namespace luminaria {

/// RAII wrapper for a Wayland global (`wl_global*`).
/// Destroys the global upon destruction. Move-only.
class WlGlobal {
    wl_global* global_ = nullptr;

public:
    WlGlobal() noexcept = default;
    explicit WlGlobal(wl_global* g) noexcept : global_(g) {}

    ~WlGlobal() { reset(); }

    WlGlobal(WlGlobal&& o) noexcept : global_(std::exchange(o.global_, nullptr)) {}
    WlGlobal& operator=(WlGlobal&& o) noexcept {
        if (this != &o) {
            reset(std::exchange(o.global_, nullptr));
        }
        return *this;
    }
    WlGlobal(const WlGlobal&) = delete;
    WlGlobal& operator=(const WlGlobal&) = delete;

    [[nodiscard]] wl_global* get() const noexcept { return global_; }
    [[nodiscard]] bool valid() const noexcept { return global_ != nullptr; }
    explicit operator bool() const noexcept { return global_ != nullptr; }

    void reset(wl_global* g = nullptr) noexcept {
        if (global_ != nullptr && global_ != g) {
            wl_global_destroy(global_);
        }
        global_ = g;
    }

    [[nodiscard]] wl_global* release() noexcept {
        return std::exchange(global_, nullptr);
    }
};

/// Universal request slot handler for requests whose only action is destroying the resource.
inline void resource_destroy_request(wl_client*, wl_resource* resource) noexcept {
    wl_resource_destroy(resource);
}

/// Generic bind function for manager/singleton globals.
/// Instantiates a `wl_resource` and assigns the provided vtable and `user_data` pointer.
template <const wl_interface* Interface, const auto* VTable, auto Deleter = nullptr>
void default_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, Interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, VTable, data, Deleter);
}

/// Creates a `wl_global` with type safety and returns an RAII `WlGlobal`.
template <const wl_interface* Interface, auto BindFn>
[[nodiscard]] inline Result<WlGlobal> create_wl_global(Display& display, uint32_t version, void* data) {
    wl_global* g = wl_global_create(display.c_ptr(), Interface, static_cast<int>(version), data, BindFn);
    if (g == nullptr) {
        return fail(std::format("wl_global_create({}) failed", Interface->name ? Interface->name : "global"));
    }
    return WlGlobal{g};
}

/// Creates a typed `wl_resource` backed by heap data `T`.
/// Ensures RAII cleanup on allocation/creation failure, and registers a type-safe deleter on destruction.
template <class T, const wl_interface* Interface, const auto* VTable>
inline wl_resource* create_user_resource(wl_client* client, uint32_t version, uint32_t id,
                                         std::unique_ptr<T> data,
                                         wl_resource* error_parent = nullptr) {
    wl_resource* resource = wl_resource_create(client, Interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        if (error_parent != nullptr) {
            wl_resource_post_no_memory(error_parent);
        } else {
            wl_client_post_no_memory(client);
        }
        return nullptr;
    }
    T* raw = data.release();
    wl_resource_set_implementation(
        resource, VTable, raw,
        [](wl_resource* r) {
            delete static_cast<T*>(wl_resource_get_user_data(r));
        });
    return resource;
}

/// Helper for protocol objects bound to a Surface lifecycle.
/// Automatically nulls `surface` when the surface is destroyed.
struct SurfaceTracker {
    Surface* surface = nullptr;
    Signal<SurfaceDestroy>::Connection destroy_conn;

    SurfaceTracker() = default;
    explicit SurfaceTracker(Surface* s) noexcept { track(s); }

    void track(Surface* s) {
        surface = s;
        if (surface != nullptr) {
            destroy_conn = surface->destroy.connect([this](SurfaceDestroy&) {
                surface = nullptr;
                on_surface_destroyed();
            });
        } else {
            destroy_conn.disconnect();
        }
    }

    virtual ~SurfaceTracker() = default;
    virtual void on_surface_destroyed() {}
};

} // namespace luminaria
