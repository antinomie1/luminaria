// luminaria/protocol/desktop/virtual_pointer.cppm — zwlr_virtual_pointer_v1.
//
// Client input injection for pointer events (relative and absolute motion,
// buttons, axis).

module;

#include <cstdint>
#include <vector>

#include "wlr-virtual-pointer-unstable-v1-protocol.h"
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

export module luminaria.desktop:virtual_pointer;

import std;

import luminaria;

export namespace luminaria {

struct VirtualPointerMotion {
  uint32_t time = 0;
  double dx = 0.0;
  double dy = 0.0;
  wl_resource *seat_resource = nullptr;
};

struct VirtualPointerMotionAbsolute {
  uint32_t time = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t x_extent = 0;
  uint32_t y_extent = 0;
  wl_resource *seat_resource = nullptr;
  wl_resource *output_resource = nullptr;
};

struct VirtualPointerButton {
  uint32_t time = 0;
  uint32_t button = 0;
  bool pressed = false;
  wl_resource *seat_resource = nullptr;
};

struct VirtualPointerAxis {
  uint32_t time = 0;
  uint32_t axis = 0;
  double value = 0.0;
  wl_resource *seat_resource = nullptr;
};

struct VirtualPointerFrame {
  wl_resource *seat_resource = nullptr;
};

class VirtualPointerManager {
public:
  [[nodiscard]] static Result<VirtualPointerManager> create(Display &display);

  ~VirtualPointerManager();
  VirtualPointerManager(VirtualPointerManager &&) noexcept;
  VirtualPointerManager &operator=(VirtualPointerManager &&) noexcept;
  VirtualPointerManager(const VirtualPointerManager &) = delete;
  VirtualPointerManager &operator=(const VirtualPointerManager &) = delete;

  [[nodiscard]] Signal<VirtualPointerMotion> &motion_event() noexcept;
  [[nodiscard]] Signal<VirtualPointerMotionAbsolute> &
  motion_absolute_event() noexcept;
  [[nodiscard]] Signal<VirtualPointerButton> &button_event() noexcept;
  [[nodiscard]] Signal<VirtualPointerAxis> &axis_event() noexcept;
  [[nodiscard]] Signal<VirtualPointerFrame> &frame_event() noexcept;

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
  explicit VirtualPointerManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// ---------------------------------------------------------------
// implementation
namespace luminaria {

struct VirtualPointerDevice;

struct VirtualPointerManager::Impl : GlobalBindingTracker {
  WlGlobal global;
  std::vector<VirtualPointerDevice *> devices;
  Signal<VirtualPointerMotion> motion_event;
  Signal<VirtualPointerMotionAbsolute> motion_absolute_event;
  Signal<VirtualPointerButton> button_event;
  Signal<VirtualPointerAxis> axis_event;
  Signal<VirtualPointerFrame> frame_event;

  ~Impl() override;
};

struct VirtualPointerDevice {
  VirtualPointerManager::Impl *mgr = nullptr;
  wl_resource *resource = nullptr;
  wl_resource *seat_resource = nullptr;
  wl_resource *output_resource = nullptr;
  wl_listener seat_destroy{};
  wl_listener output_destroy{};

  VirtualPointerDevice(VirtualPointerManager::Impl *m, wl_resource *r,
                       wl_resource *seat, wl_resource *out)
      : mgr(m), resource(r), seat_resource(seat), output_resource(out) {}
  ~VirtualPointerDevice();
};

VirtualPointerDevice::~VirtualPointerDevice() {
  if (seat_destroy.link.prev != nullptr) {
    wl_list_remove(&seat_destroy.link);
  }
  if (output_destroy.link.prev != nullptr) {
    wl_list_remove(&output_destroy.link);
  }
  if (mgr != nullptr) {
    std::erase(mgr->devices, this);
  }
}

VirtualPointerManager::Impl::~Impl() {
  for (VirtualPointerDevice *dev : devices) {
    dev->mgr = nullptr;
    detach_resource(dev->resource);
  }
}

namespace {

void vp_motion(wl_client *, wl_resource *resource, uint32_t time, wl_fixed_t dx,
               wl_fixed_t dy) {
  auto *dev =
      static_cast<VirtualPointerDevice *>(wl_resource_get_user_data(resource));
  if (dev == nullptr || dev->mgr == nullptr) {
    return;
  }
  VirtualPointerMotion event{
      .time = time,
      .dx = wl_fixed_to_double(dx),
      .dy = wl_fixed_to_double(dy),
      .seat_resource = dev->seat_resource,
  };
  dev->mgr->motion_event.emit(event);
}

void vp_motion_absolute(wl_client *, wl_resource *resource, uint32_t time,
                        uint32_t x, uint32_t y, uint32_t x_extent,
                        uint32_t y_extent) {
  auto *dev =
      static_cast<VirtualPointerDevice *>(wl_resource_get_user_data(resource));
  if (dev == nullptr || dev->mgr == nullptr) {
    return;
  }
  VirtualPointerMotionAbsolute event{
      .time = time,
      .x = x,
      .y = y,
      .x_extent = x_extent,
      .y_extent = y_extent,
      .seat_resource = dev->seat_resource,
      .output_resource = dev->output_resource,
  };
  dev->mgr->motion_absolute_event.emit(event);
}

void vp_button(wl_client *, wl_resource *resource, uint32_t time,
               uint32_t button, uint32_t state) {
  auto *dev =
      static_cast<VirtualPointerDevice *>(wl_resource_get_user_data(resource));
  if (dev == nullptr || dev->mgr == nullptr) {
    return;
  }
  VirtualPointerButton event{
      .time = time,
      .button = button,
      .pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED),
      .seat_resource = dev->seat_resource,
  };
  dev->mgr->button_event.emit(event);
}

void vp_axis(wl_client *, wl_resource *resource, uint32_t time, uint32_t axis,
             wl_fixed_t value) {
  auto *dev =
      static_cast<VirtualPointerDevice *>(wl_resource_get_user_data(resource));
  if (dev == nullptr || dev->mgr == nullptr) {
    return;
  }
  VirtualPointerAxis event{
      .time = time,
      .axis = axis,
      .value = wl_fixed_to_double(value),
      .seat_resource = dev->seat_resource,
  };
  dev->mgr->axis_event.emit(event);
}

void vp_frame(wl_client *, wl_resource *resource) {
  auto *dev =
      static_cast<VirtualPointerDevice *>(wl_resource_get_user_data(resource));
  if (dev == nullptr || dev->mgr == nullptr) {
    return;
  }
  VirtualPointerFrame event{.seat_resource = dev->seat_resource};
  dev->mgr->frame_event.emit(event);
}

void vp_axis_source(wl_client *, wl_resource *, uint32_t) {}
void vp_axis_stop(wl_client *, wl_resource *, uint32_t, uint32_t) {}
void vp_axis_discrete(wl_client *, wl_resource *, uint32_t, uint32_t,
                      wl_fixed_t, int32_t) {}

void vp_destroy(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}

constexpr struct zwlr_virtual_pointer_v1_interface vp_impl = {
    .motion = vp_motion,
    .motion_absolute = vp_motion_absolute,
    .button = vp_button,
    .axis = vp_axis,
    .frame = vp_frame,
    .axis_source = vp_axis_source,
    .axis_stop = vp_axis_stop,
    .axis_discrete = vp_axis_discrete,
    .destroy = vp_destroy,
};

void on_seat_destroyed(wl_listener *l, void *) {
  auto *dev = wl_container_of(l, static_cast<VirtualPointerDevice *>(nullptr),
                              seat_destroy);
  dev->seat_resource = nullptr;
}

void on_output_destroyed(wl_listener *l, void *) {
  auto *dev = wl_container_of(l, static_cast<VirtualPointerDevice *>(nullptr),
                              output_destroy);
  dev->output_resource = nullptr;
}

void do_create_pointer(VirtualPointerManager::Impl *self, wl_client *client,
                       uint32_t version, uint32_t id, wl_resource *seat,
                       wl_resource *output) {
  wl_resource *res =
      wl_resource_create(client, &zwlr_virtual_pointer_v1_interface,
                         static_cast<int>(version), id);
  if (res == nullptr) {
    wl_client_post_no_memory(client);
    return;
  }

  auto dev = std::make_unique<VirtualPointerDevice>(self, res, seat, output);
  if (seat != nullptr) {
    dev->seat_destroy.notify = on_seat_destroyed;
    wl_resource_add_destroy_listener(seat, &dev->seat_destroy);
  }
  if (output != nullptr) {
    dev->output_destroy.notify = on_output_destroyed;
    wl_resource_add_destroy_listener(output, &dev->output_destroy);
  }

  self->devices.push_back(dev.get());
  VirtualPointerDevice *raw = dev.release();
  wl_resource_set_implementation(res, &vp_impl, raw, [](wl_resource *r) {
    delete static_cast<VirtualPointerDevice *>(wl_resource_get_user_data(r));
  });
}

void manager_create_virtual_pointer(wl_client *client, wl_resource *resource,
                                    wl_resource *seat, uint32_t id) {
  auto *self = static_cast<VirtualPointerManager::Impl *>(
      wl_resource_get_user_data(resource));
  if (self == nullptr) {
    return;
  }
  const uint32_t version =
      static_cast<uint32_t>(wl_resource_get_version(resource));
  do_create_pointer(self, client, version, id, seat, nullptr);
}

void manager_create_virtual_pointer_with_output(wl_client *client,
                                                wl_resource *resource,
                                                wl_resource *seat,
                                                wl_resource *output,
                                                uint32_t id) {
  auto *self = static_cast<VirtualPointerManager::Impl *>(
      wl_resource_get_user_data(resource));
  if (self == nullptr) {
    return;
  }
  const uint32_t version =
      static_cast<uint32_t>(wl_resource_get_version(resource));
  do_create_pointer(self, client, version, id, seat, output);
}

constexpr struct zwlr_virtual_pointer_manager_v1_interface manager_impl = {
    .create_virtual_pointer = manager_create_virtual_pointer,
    .destroy = resource_destroy_request,
    .create_virtual_pointer_with_output =
        manager_create_virtual_pointer_with_output,
};

} // namespace

VirtualPointerManager::VirtualPointerManager(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
VirtualPointerManager::~VirtualPointerManager() = default;
VirtualPointerManager::VirtualPointerManager(
    VirtualPointerManager &&) noexcept = default;
VirtualPointerManager &
VirtualPointerManager::operator=(VirtualPointerManager &&) noexcept = default;

Signal<VirtualPointerMotion> &VirtualPointerManager::motion_event() noexcept {
  return impl_->motion_event;
}
Signal<VirtualPointerMotionAbsolute> &
VirtualPointerManager::motion_absolute_event() noexcept {
  return impl_->motion_absolute_event;
}
Signal<VirtualPointerButton> &VirtualPointerManager::button_event() noexcept {
  return impl_->button_event;
}
Signal<VirtualPointerAxis> &VirtualPointerManager::axis_event() noexcept {
  return impl_->axis_event;
}
Signal<VirtualPointerFrame> &VirtualPointerManager::frame_event() noexcept {
  return impl_->frame_event;
}

Result<VirtualPointerManager> VirtualPointerManager::create(Display &display) {
  auto impl = std::make_unique<Impl>();
  auto global = create_wl_global<
      &zwlr_virtual_pointer_manager_v1_interface,
      tracked_bind<Impl, &zwlr_virtual_pointer_manager_v1_interface,
                   &manager_impl>>(display, 2, impl.get());
  if (!global) {
    return fail(std::move(global.error().message));
  }
  impl->global = std::move(*global);
  return VirtualPointerManager{std::move(impl)};
}

} // namespace luminaria
