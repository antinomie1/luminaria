// luminaria/core/protocol_helper.cppm — reusable C++23 templates & RAII
// utilities for Wayland protocols.
//
// Eliminates repetitive protocol boilerplate (global creation, bind callbacks,
// resource lifetime & destroy handlers, surface tracking) without adding any
// runtime overhead.

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

struct SafeGlobalDestroyData {
  wl_global *global = nullptr;
  wl_event_source *timer = nullptr;
  wl_listener display_destroy{};
};

inline void do_destroy_global(SafeGlobalDestroyData *data) noexcept {
  wl_list_remove(&data->display_destroy.link);
  if (data->timer != nullptr) {
    wl_event_source_remove(data->timer);
  }
  wl_global_destroy(data->global);
  delete data;
}

/// Safely withdraw and destroy a Wayland global.
///
/// Prevents the client disconnection race condition
/// (gitlab.freedesktop.org/wayland/wayland/-/issues/10): when a global is
/// removed during runtime (e.g. monitor unplug), calling wl_global_destroy
/// immediately causes in-flight wl_registry.bind requests from clients to
/// trigger fatal protocol errors ("invalid global name").
///
/// By calling wl_global_remove first and deferring wl_global_destroy on the
/// event loop, clients safely receive wl_registry.global_remove while in-flight
/// binds succeed as inert.
inline void destroy_global_safe(wl_global *global) noexcept {
  if (global == nullptr) {
    return;
  }
  wl_global_remove(global);
  wl_global_set_user_data(global, nullptr);

  wl_display *display = wl_global_get_display(global);
  if (display == nullptr) {
    wl_global_destroy(global);
    return;
  }
  wl_event_loop *loop = wl_display_get_event_loop(display);
  if (loop == nullptr) {
    wl_global_destroy(global);
    return;
  }

  auto *data = new SafeGlobalDestroyData();
  data->global = global;
  data->display_destroy.notify = [](wl_listener *l, void *) {
    auto *d = wl_container_of(l, static_cast<SafeGlobalDestroyData *>(nullptr),
                              display_destroy);
    do_destroy_global(d);
  };
  wl_display_add_destroy_listener(display, &data->display_destroy);

  data->timer = wl_event_loop_add_timer(
      loop,
      [](void *user_data) -> int {
        auto *d = static_cast<SafeGlobalDestroyData *>(user_data);
        do_destroy_global(d);
        return 0;
      },
      data);

  if (data->timer == nullptr) {
    do_destroy_global(data);
    return;
  }
  wl_event_source_timer_update(data->timer, 5000);
}

/// RAII wrapper for a Wayland global (`wl_global*`).
/// Destroys the global upon destruction safely. Move-only.
class WlGlobal {
  wl_global *global_ = nullptr;

public:
  WlGlobal() noexcept = default;
  explicit WlGlobal(wl_global *g) noexcept : global_(g) {}

  ~WlGlobal() { reset(); }

  WlGlobal(WlGlobal &&o) noexcept
      : global_(std::exchange(o.global_, nullptr)) {}
  WlGlobal &operator=(WlGlobal &&o) noexcept {
    if (this != &o) {
      reset(std::exchange(o.global_, nullptr));
    }
    return *this;
  }
  WlGlobal(const WlGlobal &) = delete;
  WlGlobal &operator=(const WlGlobal &) = delete;

  [[nodiscard]] wl_global *get() const noexcept { return global_; }
  [[nodiscard]] bool valid() const noexcept { return global_ != nullptr; }
  explicit operator bool() const noexcept { return global_ != nullptr; }

  void reset(wl_global *g = nullptr, bool safe = true) noexcept {
    if (global_ != nullptr && global_ != g) {
      if (safe) {
        destroy_global_safe(global_);
      } else {
        wl_global_destroy(global_);
      }
    }
    global_ = g;
  }

  [[nodiscard]] wl_global *release() noexcept {
    return std::exchange(global_, nullptr);
  }
};

/// Universal request slot handler for requests whose only action is destroying
/// the resource.
inline void resource_destroy_request(wl_client *,
                                     wl_resource *resource) noexcept {
  wl_resource_destroy(resource);
}

/// Leave a client's resource inert: its user data and its destroy handler are
/// both dropped, so neither a later request nor its eventual destruction
/// reaches whatever the user data pointed at. For a global's destructor, whose
/// client objects outlive it: libwayland destroys a still-connected client's
/// resources only when the Display goes, and the Display is declared first, so
/// it goes last. Whatever the user data owned must be freed by the caller; a
/// request handler that can still run must treat null user data as "do
/// nothing".
inline void detach_resource(wl_resource *resource) noexcept {
  wl_resource_set_user_data(resource, nullptr);
  wl_resource_set_destructor(resource, nullptr);
}

/// Tracker for bound manager/global resources.
/// Automatically untracks resources when destroyed by the client,
/// and detaches all remaining resources upon destructor.
struct GlobalBindingTracker {
  std::vector<wl_resource *> bound_resources;

  virtual ~GlobalBindingTracker() {
    for (wl_resource *r : bound_resources) {
      detach_resource(r);
    }
  }

  void add_bound_resource(wl_resource *r) { bound_resources.push_back(r); }

  void remove_bound_resource(wl_resource *r) noexcept {
    std::erase(bound_resources, r);
  }
};

/// Generic bind function for manager/singleton globals.
/// Instantiates a `wl_resource` and assigns the provided vtable and `user_data`
/// pointer.
template <const wl_interface *Interface, const auto *VTable,
          auto Deleter = nullptr>
void default_bind(wl_client *client, void *data, uint32_t version,
                  uint32_t id) {
  wl_resource *resource =
      wl_resource_create(client, Interface, static_cast<int>(version), id);
  if (resource == nullptr) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(resource, VTable, data, Deleter);
}

/// Tracked bind function for manager/singleton globals whose Impl inherits
/// GlobalBindingTracker.
template <class ImplType, const wl_interface *Interface, const auto *VTable>
void tracked_bind(wl_client *client, void *data, uint32_t version,
                  uint32_t id) {
  wl_resource *resource =
      wl_resource_create(client, Interface, static_cast<int>(version), id);
  if (resource == nullptr) {
    wl_client_post_no_memory(client);
    return;
  }
  if (data == nullptr) {
    wl_resource_set_implementation(resource, VTable, nullptr, nullptr);
    return;
  }
  auto *self = static_cast<ImplType *>(data);
  self->add_bound_resource(resource);
  wl_resource_set_implementation(resource, VTable, data, [](wl_resource *r) {
    if (auto *s = static_cast<ImplType *>(wl_resource_get_user_data(r))) {
      s->remove_bound_resource(r);
    }
  });
}

/// Creates a `wl_global` with type safety and returns an RAII `WlGlobal`.
template <const wl_interface *Interface, auto BindFn>
[[nodiscard]] inline Result<WlGlobal>
create_wl_global(Display &display, uint32_t version, void *data) {
  wl_global *g = wl_global_create(display.c_ptr(), Interface,
                                  static_cast<int>(version), data, BindFn);
  if (g == nullptr) {
    return fail(std::format("wl_global_create({}) failed",
                            Interface->name ? Interface->name : "global"));
  }
  return WlGlobal{g};
}

/// Creates a typed `wl_resource` backed by heap data `T`.
/// Ensures RAII cleanup on allocation/creation failure, and registers a
/// type-safe deleter on destruction.
template <class T, const wl_interface *Interface, const auto *VTable>
inline wl_resource *create_user_resource(wl_client *client, uint32_t version,
                                         uint32_t id, std::unique_ptr<T> data,
                                         wl_resource *error_parent = nullptr) {
  wl_resource *resource =
      wl_resource_create(client, Interface, static_cast<int>(version), id);
  if (resource == nullptr) {
    if (error_parent != nullptr) {
      wl_resource_post_no_memory(error_parent);
    } else {
      wl_client_post_no_memory(client);
    }
    return nullptr;
  }
  T *raw = data.release();
  wl_resource_set_implementation(resource, VTable, raw, [](wl_resource *r) {
    delete static_cast<T *>(wl_resource_get_user_data(r));
  });
  return resource;
}

/// Helper for protocol objects bound to a Surface lifecycle.
/// Automatically nulls `surface` when the surface is destroyed.
struct SurfaceTracker {
  Surface *surface = nullptr;
  Signal<SurfaceDestroy>::Connection destroy_conn;

  SurfaceTracker() = default;
  explicit SurfaceTracker(Surface *s) noexcept { track(s); }

  void track(Surface *s) {
    surface = s;
    if (surface != nullptr) {
      destroy_conn = surface->destroy.connect([this](SurfaceDestroy &) {
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
