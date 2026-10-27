// luminaria/protocol/core/pointer_gestures.cppm — zwp_pointer_gestures_v1.
//
// Touchpad gestures: swipe, pinch, and hold.

module;

#include <cstdint>
#include <vector>

#include "pointer-gestures-unstable-v1-protocol.h"
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

export module luminaria:pointer_gestures;

import std;

import :compositor;
import :display;
import :expected;
import :protocol_helper;
import :signal;

export namespace luminaria {

class Display;
class Surface;

class PointerGestures {
public:
  [[nodiscard]] static Result<PointerGestures> create(Display &display);

  ~PointerGestures();
  PointerGestures(PointerGestures &&) noexcept;
  PointerGestures &operator=(PointerGestures &&) noexcept;
  PointerGestures(const PointerGestures &) = delete;
  PointerGestures &operator=(const PointerGestures &) = delete;

  void send_swipe_begin(Surface &surface, uint32_t serial, uint32_t time_ms,
                        uint32_t fingers);
  void send_swipe_update(uint32_t time_ms, double dx, double dy);
  void send_swipe_end(uint32_t serial, uint32_t time_ms, bool cancelled);

  void send_pinch_begin(Surface &surface, uint32_t serial, uint32_t time_ms,
                        uint32_t fingers);
  void send_pinch_update(uint32_t time_ms, double dx, double dy, double scale,
                         double rotation_deg);
  void send_pinch_end(uint32_t serial, uint32_t time_ms, bool cancelled);

  void send_hold_begin(Surface &surface, uint32_t serial, uint32_t time_ms,
                       uint32_t fingers);
  void send_hold_end(uint32_t serial, uint32_t time_ms, bool cancelled);

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
  explicit PointerGestures(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// ---------------------------------------------------------------
// implementation
namespace luminaria {

struct SwipeGesture {
  PointerGestures::Impl *mgr = nullptr;
  wl_resource *resource = nullptr;
  wl_resource *pointer = nullptr;

  SwipeGesture(PointerGestures::Impl *m, wl_resource *r, wl_resource *p)
      : mgr(m), resource(r), pointer(p) {}
  ~SwipeGesture();
};

struct PinchGesture {
  PointerGestures::Impl *mgr = nullptr;
  wl_resource *resource = nullptr;
  wl_resource *pointer = nullptr;

  PinchGesture(PointerGestures::Impl *m, wl_resource *r, wl_resource *p)
      : mgr(m), resource(r), pointer(p) {}
  ~PinchGesture();
};

struct HoldGesture {
  PointerGestures::Impl *mgr = nullptr;
  wl_resource *resource = nullptr;
  wl_resource *pointer = nullptr;

  HoldGesture(PointerGestures::Impl *m, wl_resource *r, wl_resource *p)
      : mgr(m), resource(r), pointer(p) {}
  ~HoldGesture();
};

struct PointerGestures::Impl : GlobalBindingTracker {
  WlGlobal global;
  std::vector<SwipeGesture *> swipe_gestures;
  std::vector<PinchGesture *> pinch_gestures;
  std::vector<HoldGesture *> hold_gestures;
  Surface *current_surface = nullptr;
  Signal<SurfaceDestroy>::Connection surface_destroy;

  ~Impl() override {
    for (SwipeGesture *g : swipe_gestures) {
      g->mgr = nullptr;
      detach_resource(g->resource);
    }
    for (PinchGesture *g : pinch_gestures) {
      g->mgr = nullptr;
      detach_resource(g->resource);
    }
    for (HoldGesture *g : hold_gestures) {
      g->mgr = nullptr;
      detach_resource(g->resource);
    }
  }
};

SwipeGesture::~SwipeGesture() {
  if (mgr != nullptr) {
    std::erase(mgr->swipe_gestures, this);
  }
}

PinchGesture::~PinchGesture() {
  if (mgr != nullptr) {
    std::erase(mgr->pinch_gestures, this);
  }
}

HoldGesture::~HoldGesture() {
  if (mgr != nullptr) {
    std::erase(mgr->hold_gestures, this);
  }
}

namespace {

constexpr struct zwp_pointer_gesture_swipe_v1_interface swipe_impl = {
    .destroy = resource_destroy_request,
};

constexpr struct zwp_pointer_gesture_pinch_v1_interface pinch_impl = {
    .destroy = resource_destroy_request,
};

constexpr struct zwp_pointer_gesture_hold_v1_interface hold_impl = {
    .destroy = resource_destroy_request,
};

void gestures_get_swipe_gesture(wl_client *client, wl_resource *resource,
                                uint32_t id, wl_resource *pointer) {
  auto *self =
      static_cast<PointerGestures::Impl *>(wl_resource_get_user_data(resource));
  if (self == nullptr) {
    return;
  }
  const uint32_t version =
      static_cast<uint32_t>(wl_resource_get_version(resource));
  wl_resource *res =
      wl_resource_create(client, &zwp_pointer_gesture_swipe_v1_interface,
                         static_cast<int>(version), id);
  if (res == nullptr) {
    wl_client_post_no_memory(client);
    return;
  }
  auto obj = std::make_unique<SwipeGesture>(self, res, pointer);
  self->swipe_gestures.push_back(obj.get());
  SwipeGesture *raw = obj.release();
  wl_resource_set_implementation(res, &swipe_impl, raw, [](wl_resource *r) {
    delete static_cast<SwipeGesture *>(wl_resource_get_user_data(r));
  });
}

void gestures_get_pinch_gesture(wl_client *client, wl_resource *resource,
                                uint32_t id, wl_resource *pointer) {
  auto *self =
      static_cast<PointerGestures::Impl *>(wl_resource_get_user_data(resource));
  if (self == nullptr) {
    return;
  }
  const uint32_t version =
      static_cast<uint32_t>(wl_resource_get_version(resource));
  wl_resource *res =
      wl_resource_create(client, &zwp_pointer_gesture_pinch_v1_interface,
                         static_cast<int>(version), id);
  if (res == nullptr) {
    wl_client_post_no_memory(client);
    return;
  }
  auto obj = std::make_unique<PinchGesture>(self, res, pointer);
  self->pinch_gestures.push_back(obj.get());
  PinchGesture *raw = obj.release();
  wl_resource_set_implementation(res, &pinch_impl, raw, [](wl_resource *r) {
    delete static_cast<PinchGesture *>(wl_resource_get_user_data(r));
  });
}

void gestures_release(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}

void gestures_get_hold_gesture(wl_client *client, wl_resource *resource,
                               uint32_t id, wl_resource *pointer) {
  auto *self =
      static_cast<PointerGestures::Impl *>(wl_resource_get_user_data(resource));
  if (self == nullptr) {
    return;
  }
  const uint32_t version =
      static_cast<uint32_t>(wl_resource_get_version(resource));
  wl_resource *res =
      wl_resource_create(client, &zwp_pointer_gesture_hold_v1_interface,
                         static_cast<int>(version), id);
  if (res == nullptr) {
    wl_client_post_no_memory(client);
    return;
  }
  auto obj = std::make_unique<HoldGesture>(self, res, pointer);
  self->hold_gestures.push_back(obj.get());
  HoldGesture *raw = obj.release();
  wl_resource_set_implementation(res, &hold_impl, raw, [](wl_resource *r) {
    delete static_cast<HoldGesture *>(wl_resource_get_user_data(r));
  });
}

constexpr struct zwp_pointer_gestures_v1_interface gestures_impl = {
    .get_swipe_gesture = gestures_get_swipe_gesture,
    .get_pinch_gesture = gestures_get_pinch_gesture,
    .release = gestures_release,
    .get_hold_gesture = gestures_get_hold_gesture,
};

} // namespace

PointerGestures::PointerGestures(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
PointerGestures::~PointerGestures() = default;
PointerGestures::PointerGestures(PointerGestures &&) noexcept = default;
PointerGestures &
PointerGestures::operator=(PointerGestures &&) noexcept = default;

Result<PointerGestures> PointerGestures::create(Display &display) {
  auto impl = std::make_unique<Impl>();
  auto global = create_wl_global<
      &zwp_pointer_gestures_v1_interface,
      tracked_bind<Impl, &zwp_pointer_gestures_v1_interface, &gestures_impl>>(
      display, 3, impl.get());
  if (!global) {
    return fail(std::move(global.error().message));
  }
  impl->global = std::move(*global);
  return PointerGestures{std::move(impl)};
}

void PointerGestures::send_swipe_begin(Surface &surface, uint32_t serial,
                                       uint32_t time_ms, uint32_t fingers) {
  impl_->current_surface = &surface;
  impl_->surface_destroy =
      surface.destroy.connect([raw = impl_.get()](SurfaceDestroy &) {
        raw->current_surface = nullptr;
      });
  wl_resource *surf_res = surface.c_resource();
  if (surf_res == nullptr) {
    return;
  }
  wl_client *target_client = wl_resource_get_client(surf_res);
  for (SwipeGesture *g : impl_->swipe_gestures) {
    if (wl_resource_get_client(g->resource) == target_client) {
      zwp_pointer_gesture_swipe_v1_send_begin(g->resource, serial, time_ms,
                                              surf_res, fingers);
    }
  }
}

void PointerGestures::send_swipe_update(uint32_t time_ms, double dx,
                                        double dy) {
  if (impl_->current_surface == nullptr) {
    return;
  }
  wl_resource *surf_res = impl_->current_surface->c_resource();
  if (surf_res == nullptr) {
    return;
  }
  wl_client *target_client = wl_resource_get_client(surf_res);
  for (SwipeGesture *g : impl_->swipe_gestures) {
    if (wl_resource_get_client(g->resource) == target_client) {
      zwp_pointer_gesture_swipe_v1_send_update(g->resource, time_ms,
                                               wl_fixed_from_double(dx),
                                               wl_fixed_from_double(dy));
    }
  }
}

void PointerGestures::send_swipe_end(uint32_t serial, uint32_t time_ms,
                                     bool cancelled) {
  if (impl_->current_surface == nullptr) {
    return;
  }
  wl_resource *surf_res = impl_->current_surface->c_resource();
  impl_->current_surface = nullptr;
  impl_->surface_destroy.disconnect();
  if (surf_res == nullptr) {
    return;
  }
  wl_client *target_client = wl_resource_get_client(surf_res);
  for (SwipeGesture *g : impl_->swipe_gestures) {
    if (wl_resource_get_client(g->resource) == target_client) {
      zwp_pointer_gesture_swipe_v1_send_end(g->resource, serial, time_ms,
                                            cancelled ? 1 : 0);
    }
  }
}

void PointerGestures::send_pinch_begin(Surface &surface, uint32_t serial,
                                       uint32_t time_ms, uint32_t fingers) {
  impl_->current_surface = &surface;
  impl_->surface_destroy =
      surface.destroy.connect([raw = impl_.get()](SurfaceDestroy &) {
        raw->current_surface = nullptr;
      });
  wl_resource *surf_res = surface.c_resource();
  if (surf_res == nullptr) {
    return;
  }
  wl_client *target_client = wl_resource_get_client(surf_res);
  for (PinchGesture *g : impl_->pinch_gestures) {
    if (wl_resource_get_client(g->resource) == target_client) {
      zwp_pointer_gesture_pinch_v1_send_begin(g->resource, serial, time_ms,
                                              surf_res, fingers);
    }
  }
}

void PointerGestures::send_pinch_update(uint32_t time_ms, double dx, double dy,
                                        double scale, double rotation_deg) {
  if (impl_->current_surface == nullptr) {
    return;
  }
  wl_resource *surf_res = impl_->current_surface->c_resource();
  if (surf_res == nullptr) {
    return;
  }
  wl_client *target_client = wl_resource_get_client(surf_res);
  for (PinchGesture *g : impl_->pinch_gestures) {
    if (wl_resource_get_client(g->resource) == target_client) {
      zwp_pointer_gesture_pinch_v1_send_update(
          g->resource, time_ms, wl_fixed_from_double(dx),
          wl_fixed_from_double(dy), wl_fixed_from_double(scale),
          wl_fixed_from_double(rotation_deg));
    }
  }
}

void PointerGestures::send_pinch_end(uint32_t serial, uint32_t time_ms,
                                     bool cancelled) {
  if (impl_->current_surface == nullptr) {
    return;
  }
  wl_resource *surf_res = impl_->current_surface->c_resource();
  impl_->current_surface = nullptr;
  impl_->surface_destroy.disconnect();
  if (surf_res == nullptr) {
    return;
  }
  wl_client *target_client = wl_resource_get_client(surf_res);
  for (PinchGesture *g : impl_->pinch_gestures) {
    if (wl_resource_get_client(g->resource) == target_client) {
      zwp_pointer_gesture_pinch_v1_send_end(g->resource, serial, time_ms,
                                            cancelled ? 1 : 0);
    }
  }
}

void PointerGestures::send_hold_begin(Surface &surface, uint32_t serial,
                                      uint32_t time_ms, uint32_t fingers) {
  impl_->current_surface = &surface;
  impl_->surface_destroy =
      surface.destroy.connect([raw = impl_.get()](SurfaceDestroy &) {
        raw->current_surface = nullptr;
      });
  wl_resource *surf_res = surface.c_resource();
  if (surf_res == nullptr) {
    return;
  }
  wl_client *target_client = wl_resource_get_client(surf_res);
  for (HoldGesture *g : impl_->hold_gestures) {
    if (wl_resource_get_client(g->resource) == target_client) {
      zwp_pointer_gesture_hold_v1_send_begin(g->resource, serial, time_ms,
                                             surf_res, fingers);
    }
  }
}

void PointerGestures::send_hold_end(uint32_t serial, uint32_t time_ms,
                                    bool cancelled) {
  if (impl_->current_surface == nullptr) {
    return;
  }
  wl_resource *surf_res = impl_->current_surface->c_resource();
  impl_->current_surface = nullptr;
  impl_->surface_destroy.disconnect();
  if (surf_res == nullptr) {
    return;
  }
  wl_client *target_client = wl_resource_get_client(surf_res);
  for (HoldGesture *g : impl_->hold_gestures) {
    if (wl_resource_get_client(g->resource) == target_client) {
      zwp_pointer_gesture_hold_v1_send_end(g->resource, serial, time_ms,
                                           cancelled ? 1 : 0);
    }
  }
}

} // namespace luminaria
