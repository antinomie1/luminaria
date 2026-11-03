// luminaria/protocol/desktop/gamma_control.cppm — zwlr_gamma_control_v1.
//
// Allows a client (night light / gamma adjuster) to set gamma ramps on outputs.
// Tables differ per output, so the compositor says how big each one is
// (`set_output_size`, by `OutputGlobal::identity()`); a client asking for an
// output with no table gets `failed`. Applying the ramp is the compositor's:
// `Output::set_gamma` does it on DRM.

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
  /// The client's wl_output; null on a reset whose wl_output is already gone.
  wl_resource *output_resource = nullptr;
  /// Which output, as `OutputGlobal::identity()`: still right on that reset.
  const void *output_identity = nullptr;
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

  /// The entries per channel the output with `identity` takes; 0 (the
  /// default for an output never named) means it has no gamma table.
  void set_output_size(const void *identity, uint32_t gamma_size);
  /// The `OutputGlobal::identity()` a client's wl_output names, as
  /// `GammaRampUpdate::output_resource` carries it; null for a gone output.
  [[nodiscard]] static const void *output_identity(wl_resource *wl_output) noexcept;

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
  /// Set by the compositor; an output missing from here falls back to
  /// `gamma_size` only while nothing was set at all.
  std::vector<std::pair<const void *, uint32_t>> sizes;
  std::vector<GammaControl *> controls;

  [[nodiscard]] uint32_t size_for(wl_resource *output) const {
    if (sizes.empty()) {
      return gamma_size;
    }
    const void *identity = OutputGlobal::identity_of(output);
    for (const auto &[id, size] : sizes) {
      if (id == identity) {
        return size;
      }
    }
    return 0;
  }
  Signal<GammaRampUpdate> gamma_changed;

  ~Impl() override;
};

struct GammaControl {
  GammaControlManager::Impl *mgr = nullptr;
  wl_resource *resource = nullptr;
  wl_resource *output_resource = nullptr;
  wl_listener output_destroy{};
  uint32_t gamma_size = 0;
  const void *identity = nullptr;
  /// A ramp of ours is on the output: our end must reset it.
  bool applied = false;

  /// Hands the output back to the identity ramp, once.
  void reset() {
    if (applied && mgr != nullptr) {
      applied = false;
      GammaRampUpdate event{output_resource, identity, std::nullopt};
      mgr->gamma_changed.emit(event);
    }
  }

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
    // A night-light tool quitting, or crashing, leaves the screen as it was.
    reset();
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

  const uint32_t gamma_size = self->gamma_size;
  if (gamma_size == 0 || self->output_resource == nullptr) {
    if (fd >= 0) {
      ::close(fd);
    }
    return; // already told `failed`
  }
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

  self->applied = true;
  GammaRampUpdate event{self->output_resource, self->identity, std::move(ramp)};
  self->mgr->gamma_changed.emit(event);
}

constexpr struct zwlr_gamma_control_v1_interface control_impl = {
    .set_gamma = control_set_gamma,
    .destroy = resource_destroy_request,
};

void on_output_destroyed(wl_listener *l, void *) {
  auto *ctrl =
      wl_container_of(l, static_cast<GammaControl *>(nullptr), output_destroy);
  if (ctrl->output_resource != nullptr) {
    zwlr_gamma_control_v1_send_failed(ctrl->resource);
    ctrl->output_resource = nullptr;
    ctrl->reset();
  }
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
      // The newcomer owns the output now; the old ramp stays until it sets one.
      existing->applied = false;
    }
  }

  auto ctrl = std::make_unique<GammaControl>(self, res, output);
  ctrl->output_destroy.notify = on_output_destroyed;
  wl_resource_add_destroy_listener(output, &ctrl->output_destroy);
  ctrl->gamma_size = self->size_for(output);
  ctrl->identity = OutputGlobal::identity_of(output);

  self->controls.push_back(ctrl.get());
  GammaControl *raw = ctrl.release();
  wl_resource_set_implementation(res, &control_impl, raw, [](wl_resource *r) {
    delete static_cast<GammaControl *>(wl_resource_get_user_data(r));
  });

  if (raw->gamma_size == 0) {
    // This output has no table to set; the client should not wait for one.
    zwlr_gamma_control_v1_send_failed(res);
    raw->output_resource = nullptr;
    return;
  }
  zwlr_gamma_control_v1_send_gamma_size(res, raw->gamma_size);
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

void GammaControlManager::set_output_size(const void *identity, uint32_t gamma_size) {
  auto &sizes = impl_->sizes;
  std::erase_if(sizes, [identity](const auto &entry) { return entry.first == identity; });
  sizes.emplace_back(identity, gamma_size);
}

const void *GammaControlManager::output_identity(wl_resource *wl_output) noexcept {
  return OutputGlobal::identity_of(wl_output);
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
