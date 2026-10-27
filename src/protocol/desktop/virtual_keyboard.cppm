// luminaria/protocol/desktop/virtual_keyboard.cppm — zwp_virtual_keyboard_v1.
//
// Client input injection for keyboard events.

module;

#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "virtual-keyboard-unstable-v1-protocol.h"
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

export module luminaria.desktop:virtual_keyboard;

import std;

import luminaria;

export namespace luminaria {

struct VirtualKey {
  uint32_t time = 0;
  uint32_t key = 0;
  bool pressed = false;
  wl_resource *seat_resource = nullptr;
};

struct VirtualModifiers {
  uint32_t depressed = 0;
  uint32_t latched = 0;
  uint32_t locked = 0;
  uint32_t group = 0;
  wl_resource *seat_resource = nullptr;
};

class VirtualKeyboardManager {
public:
  [[nodiscard]] static Result<VirtualKeyboardManager> create(Display &display);

  ~VirtualKeyboardManager();
  VirtualKeyboardManager(VirtualKeyboardManager &&) noexcept;
  VirtualKeyboardManager &operator=(VirtualKeyboardManager &&) noexcept;
  VirtualKeyboardManager(const VirtualKeyboardManager &) = delete;
  VirtualKeyboardManager &operator=(const VirtualKeyboardManager &) = delete;

  [[nodiscard]] Signal<VirtualKey> &key_event() noexcept;
  [[nodiscard]] Signal<VirtualModifiers> &modifiers_event() noexcept;

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
  explicit VirtualKeyboardManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// ---------------------------------------------------------------
// implementation
namespace luminaria {

struct VirtualKeyboardDevice;

struct VirtualKeyboardManager::Impl : GlobalBindingTracker {
  WlGlobal global;
  std::vector<VirtualKeyboardDevice *> devices;
  Signal<VirtualKey> key_event;
  Signal<VirtualModifiers> modifiers_event;

  ~Impl() override;
};

struct VirtualKeyboardDevice {
  VirtualKeyboardManager::Impl *mgr = nullptr;
  wl_resource *resource = nullptr;
  wl_resource *seat_resource = nullptr;
  wl_listener seat_destroy{};

  VirtualKeyboardDevice(VirtualKeyboardManager::Impl *m, wl_resource *r,
                        wl_resource *seat)
      : mgr(m), resource(r), seat_resource(seat) {}
  ~VirtualKeyboardDevice();
};

VirtualKeyboardDevice::~VirtualKeyboardDevice() {
  if (seat_destroy.link.prev != nullptr) {
    wl_list_remove(&seat_destroy.link);
  }
  if (mgr != nullptr) {
    std::erase(mgr->devices, this);
  }
}

VirtualKeyboardManager::Impl::~Impl() {
  for (VirtualKeyboardDevice *dev : devices) {
    dev->mgr = nullptr;
    detach_resource(dev->resource);
  }
}

namespace {

void vk_keymap(wl_client *, wl_resource *resource, uint32_t, int32_t fd,
               uint32_t) {
  if (fd >= 0) {
    ::close(fd);
  }
  (void)resource;
}

void vk_key(wl_client *, wl_resource *resource, uint32_t time, uint32_t key,
            uint32_t state) {
  auto *dev =
      static_cast<VirtualKeyboardDevice *>(wl_resource_get_user_data(resource));
  if (dev == nullptr || dev->mgr == nullptr) {
    return;
  }
  VirtualKey event{
      .time = time,
      .key = key,
      .pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED),
      .seat_resource = dev->seat_resource,
  };
  dev->mgr->key_event.emit(event);
}

void vk_modifiers(wl_client *, wl_resource *resource, uint32_t mods_depressed,
                  uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
  auto *dev =
      static_cast<VirtualKeyboardDevice *>(wl_resource_get_user_data(resource));
  if (dev == nullptr || dev->mgr == nullptr) {
    return;
  }
  VirtualModifiers event{
      .depressed = mods_depressed,
      .latched = mods_latched,
      .locked = mods_locked,
      .group = group,
      .seat_resource = dev->seat_resource,
  };
  dev->mgr->modifiers_event.emit(event);
}

void vk_destroy(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}

constexpr struct zwp_virtual_keyboard_v1_interface vk_impl = {
    .keymap = vk_keymap,
    .key = vk_key,
    .modifiers = vk_modifiers,
    .destroy = vk_destroy,
};

void on_seat_destroyed(wl_listener *l, void *) {
  auto *dev = wl_container_of(l, static_cast<VirtualKeyboardDevice *>(nullptr),
                              seat_destroy);
  dev->seat_resource = nullptr;
}

void manager_create_virtual_keyboard(wl_client *client, wl_resource *resource,
                                     wl_resource *seat, uint32_t id) {
  auto *self = static_cast<VirtualKeyboardManager::Impl *>(
      wl_resource_get_user_data(resource));
  if (self == nullptr) {
    return;
  }
  const uint32_t version =
      static_cast<uint32_t>(wl_resource_get_version(resource));
  wl_resource *res =
      wl_resource_create(client, &zwp_virtual_keyboard_v1_interface,
                         static_cast<int>(version), id);
  if (res == nullptr) {
    wl_client_post_no_memory(client);
    return;
  }

  auto dev = std::make_unique<VirtualKeyboardDevice>(self, res, seat);
  if (seat != nullptr) {
    dev->seat_destroy.notify = on_seat_destroyed;
    wl_resource_add_destroy_listener(seat, &dev->seat_destroy);
  }

  self->devices.push_back(dev.get());
  VirtualKeyboardDevice *raw = dev.release();
  wl_resource_set_implementation(res, &vk_impl, raw, [](wl_resource *r) {
    delete static_cast<VirtualKeyboardDevice *>(wl_resource_get_user_data(r));
  });
}

constexpr struct zwp_virtual_keyboard_manager_v1_interface manager_impl = {
    .create_virtual_keyboard = manager_create_virtual_keyboard,
};

} // namespace

VirtualKeyboardManager::VirtualKeyboardManager(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
VirtualKeyboardManager::~VirtualKeyboardManager() = default;
VirtualKeyboardManager::VirtualKeyboardManager(
    VirtualKeyboardManager &&) noexcept = default;
VirtualKeyboardManager &
VirtualKeyboardManager::operator=(VirtualKeyboardManager &&) noexcept = default;

Signal<VirtualKey> &VirtualKeyboardManager::key_event() noexcept {
  return impl_->key_event;
}

Signal<VirtualModifiers> &VirtualKeyboardManager::modifiers_event() noexcept {
  return impl_->modifiers_event;
}

Result<VirtualKeyboardManager>
VirtualKeyboardManager::create(Display &display) {
  auto impl = std::make_unique<Impl>();
  auto global = create_wl_global<
      &zwp_virtual_keyboard_manager_v1_interface,
      tracked_bind<Impl, &zwp_virtual_keyboard_manager_v1_interface,
                   &manager_impl>>(display, 1, impl.get());
  if (!global) {
    return fail(std::move(global.error().message));
  }
  impl->global = std::move(*global);
  return VirtualKeyboardManager{std::move(impl)};
}

} // namespace luminaria
