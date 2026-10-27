// luminaria/protocol/desktop/gamma_control.cppm — zwlr_gamma_control_v1.
//
// Allows a client (night light / gamma adjuster) to set gamma ramps on outputs.

module;

#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "wlr-gamma-control-unstable-v1-protocol.h"
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

export module luminaria.desktop:gamma_control;

import std;

import luminaria;

export namespace luminaria {

struct GammaRamp {
  std::vector<uint16_t> red;
  std::vector<uint16_t> green;
  std::vector<uint16_t> blue;
};

struct GammaRampUpdate {
  wl_resource *output_resource = nullptr;
  std::optional<GammaRamp>
      ramp; ///< nullopt indicates gamma was reset to default
};

class GammaControlManager {
public:
  [[nodiscard]] static Result<GammaControlManager>
  create(Display &display, uint32_t gamma_size = 256);

  ~GammaControlManager();
  GammaControlManager(GammaControlManager &&) noexcept;
  GammaControlManager &operator=(GammaControlManager &&) noexcept;
  GammaControlManager(const GammaControlManager &) = delete;
  GammaControlManager &operator=(const GammaControlManager &) = delete;

  [[nodiscard]] Signal<GammaRampUpdate> &gamma_changed() noexcept;

  [[nodiscard]] uint32_t gamma_size() const noexcept;

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
  explicit GammaControlManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// ---------------------------------------------------------------
// implementation
namespace luminaria {

struct GammaControl;

struct GammaControlManager::Impl : GlobalBindingTracker {
  WlGlobal global;
  uint32_t gamma_size = 256;
  std::vector<GammaControl *> controls;
  Signal<GammaRampUpdate> gamma_changed;

  ~Impl() override;
};

struct GammaControl {
  GammaControlManager::Impl *mgr = nullptr;
  wl_resource *resource = nullptr;
  wl_resource *output_resource = nullptr;
  wl_listener output_destroy{};

  GammaControl(GammaControlManager::Impl *m, wl_resource *r, wl_resource *out)
      : mgr(m), resource(r), output_resource(out) {}
  ~GammaControl();
};

GammaControl::~GammaControl() {
  if (output_destroy.link.prev != nullptr) {
    wl_list_remove(&output_destroy.link);
  }
  if (mgr != nullptr) {
    std::erase(mgr->controls, this);
    if (output_resource != nullptr) {
      GammaRampUpdate event{output_resource, std::nullopt};
      mgr->gamma_changed.emit(event);
    }
  }
}

GammaControlManager::Impl::~Impl() {
  for (GammaControl *c : controls) {
    c->mgr = nullptr;
    detach_resource(c->resource);
  }
}

namespace {

void control_set_gamma(wl_client *, wl_resource *resource, int32_t fd) {
  auto *self = static_cast<GammaControl *>(wl_resource_get_user_data(resource));
  if (self == nullptr || self->mgr == nullptr) {
    if (fd >= 0) {
      ::close(fd);
    }
    return;
  }

  const uint32_t gamma_size = self->mgr->gamma_size;
  const size_t expected_size =
      static_cast<size_t>(gamma_size) * 3 * sizeof(uint16_t);

  if (fd < 0) {
    wl_resource_post_error(resource, ZWLR_GAMMA_CONTROL_V1_ERROR_INVALID_GAMMA,
                           "invalid file descriptor for gamma ramp");
    return;
  }

  const off_t size = ::lseek(fd, 0, SEEK_END);
  if (size < 0 || static_cast<size_t>(size) != expected_size) {
    ::close(fd);
    wl_resource_post_error(
        resource, ZWLR_GAMMA_CONTROL_V1_ERROR_INVALID_GAMMA,
        "gamma file size does not match expected size for gamma_size");
    return;
  }

  void *map = ::mmap(nullptr, expected_size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);

  if (map == MAP_FAILED) {
    wl_resource_post_error(resource, ZWLR_GAMMA_CONTROL_V1_ERROR_INVALID_GAMMA,
                           "mmap failed on gamma fd");
    return;
  }

  const auto *words = static_cast<const uint16_t *>(map);
  GammaRamp ramp;
  ramp.red.assign(words, words + gamma_size);
  ramp.green.assign(words + gamma_size, words + 2 * gamma_size);
  ramp.blue.assign(words + 2 * gamma_size, words + 3 * gamma_size);
  ::munmap(map, expected_size);

  GammaRampUpdate event{self->output_resource, std::move(ramp)};
  self->mgr->gamma_changed.emit(event);
}

constexpr struct zwlr_gamma_control_v1_interface control_impl = {
    .set_gamma = control_set_gamma,
    .destroy = resource_destroy_request,
};

void on_output_destroyed(wl_listener *l, void *) {
  auto *ctrl =
      wl_container_of(l, static_cast<GammaControl *>(nullptr), output_destroy);
  zwlr_gamma_control_v1_send_failed(ctrl->resource);
  ctrl->output_resource = nullptr;
}

void manager_get_gamma_control(wl_client *client, wl_resource *resource,
                               uint32_t id, wl_resource *output) {
  auto *self = static_cast<GammaControlManager::Impl *>(
      wl_resource_get_user_data(resource));
  if (self == nullptr) {
    return;
  }
  const uint32_t version =
      static_cast<uint32_t>(wl_resource_get_version(resource));
  wl_resource *res = wl_resource_create(
      client, &zwlr_gamma_control_v1_interface, static_cast<int>(version), id);
  if (res == nullptr) {
    wl_client_post_no_memory(client);
    return;
  }

  // Only one gamma control per output: supersede any existing control for this
  // output
  for (GammaControl *existing : self->controls) {
    if (existing->output_resource == output) {
      zwlr_gamma_control_v1_send_failed(existing->resource);
      existing->output_resource = nullptr;
    }
  }

  auto ctrl = std::make_unique<GammaControl>(self, res, output);
  ctrl->output_destroy.notify = on_output_destroyed;
  wl_resource_add_destroy_listener(output, &ctrl->output_destroy);

  self->controls.push_back(ctrl.get());
  GammaControl *raw = ctrl.release();
  wl_resource_set_implementation(res, &control_impl, raw, [](wl_resource *r) {
    delete static_cast<GammaControl *>(wl_resource_get_user_data(r));
  });

  zwlr_gamma_control_v1_send_gamma_size(res, self->gamma_size);
}

constexpr struct zwlr_gamma_control_manager_v1_interface manager_impl = {
    .get_gamma_control = manager_get_gamma_control,
    .destroy = resource_destroy_request,
};

} // namespace

GammaControlManager::GammaControlManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
GammaControlManager::~GammaControlManager() = default;
GammaControlManager::GammaControlManager(GammaControlManager &&) noexcept =
    default;
GammaControlManager &
GammaControlManager::operator=(GammaControlManager &&) noexcept = default;

Signal<GammaRampUpdate> &GammaControlManager::gamma_changed() noexcept {
  return impl_->gamma_changed;
}

uint32_t GammaControlManager::gamma_size() const noexcept {
  return impl_->gamma_size;
}

Result<GammaControlManager> GammaControlManager::create(Display &display,
                                                        uint32_t gamma_size) {
  auto impl = std::make_unique<Impl>();
  impl->gamma_size = gamma_size;
  auto global = create_wl_global<
      &zwlr_gamma_control_manager_v1_interface,
      tracked_bind<Impl, &zwlr_gamma_control_manager_v1_interface,
                   &manager_impl>>(display, 1, impl.get());
  if (!global) {
    return fail(std::move(global.error().message));
  }
  impl->global = std::move(*global);
  return GammaControlManager{std::move(impl)};
}

} // namespace luminaria
