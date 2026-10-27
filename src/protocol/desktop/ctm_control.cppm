// luminaria/protocol/desktop/ctm_control.cppm — hyprland_ctm_control_v1.
//
// Color Transform Matrix (CTM) control protocol for outputs.

module;

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "hyprland-ctm-control-v1-protocol.h"
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

export module luminaria.desktop:ctm_control;

import std;

import luminaria;

export namespace luminaria {

using ColorTransformMatrix = std::array<double, 9>;

constexpr ColorTransformMatrix identity_ctm() noexcept {
  return {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
}

struct CtmUpdate {
  wl_resource *output_resource = nullptr;
  ColorTransformMatrix matrix = identity_ctm();
};

class CtmControlManager {
public:
  [[nodiscard]] static Result<CtmControlManager> create(Display &display);

  ~CtmControlManager();
  CtmControlManager(CtmControlManager &&) noexcept;
  CtmControlManager &operator=(CtmControlManager &&) noexcept;
  CtmControlManager(const CtmControlManager &) = delete;
  CtmControlManager &operator=(const CtmControlManager &) = delete;

  [[nodiscard]] Signal<CtmUpdate> &ctm_changed() noexcept;

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
  explicit CtmControlManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// ---------------------------------------------------------------
// implementation
namespace luminaria {

struct CtmClient {
  CtmControlManager::Impl *mgr = nullptr;
  wl_resource *resource = nullptr;
  bool blocked = false;
  std::map<wl_resource *, ColorTransformMatrix> pending;

  CtmClient(CtmControlManager::Impl *m, wl_resource *r, bool b)
      : mgr(m), resource(r), blocked(b) {}
  ~CtmClient();
};

struct CtmControlManager::Impl : GlobalBindingTracker {
  WlGlobal global;
  CtmClient *active_client = nullptr;
  std::vector<CtmClient *> all_clients;
  Signal<CtmUpdate> ctm_changed;

  ~Impl() override {
    for (CtmClient *c : all_clients) {
      c->mgr = nullptr;
      detach_resource(c->resource);
    }
  }
};

CtmClient::~CtmClient() {
  if (mgr != nullptr) {
    std::erase(mgr->all_clients, this);
    if (mgr->active_client == this) {
      mgr->active_client = nullptr;
      // Reset CTMs back to identity
      for (const auto &[out_res, _] : pending) {
        CtmUpdate event{out_res, identity_ctm()};
        mgr->ctm_changed.emit(event);
      }
    }
  }
}

namespace {

void ctm_set_ctm_for_output(wl_client *, wl_resource *resource,
                            wl_resource *output, wl_fixed_t mat0,
                            wl_fixed_t mat1, wl_fixed_t mat2, wl_fixed_t mat3,
                            wl_fixed_t mat4, wl_fixed_t mat5, wl_fixed_t mat6,
                            wl_fixed_t mat7, wl_fixed_t mat8) {
  auto *client = static_cast<CtmClient *>(wl_resource_get_user_data(resource));
  if (client == nullptr || client->blocked || client->mgr == nullptr) {
    return;
  }

  const std::array<wl_fixed_t, 9> fixed_vals = {mat0, mat1, mat2, mat3, mat4,
                                                mat5, mat6, mat7, mat8};
  ColorTransformMatrix mat{};
  for (size_t i = 0; i < 9; ++i) {
    const double d = wl_fixed_to_double(fixed_vals[i]);
    if (d < 0.0) {
      wl_resource_post_error(
          resource, HYPRLAND_CTM_CONTROL_MANAGER_V1_ERROR_INVALID_MATRIX,
          "CTM matrix values must be non-negative");
      return;
    }
    mat[i] = d;
  }

  client->pending[output] = mat;
}

void ctm_commit(wl_client *, wl_resource *resource) {
  auto *client = static_cast<CtmClient *>(wl_resource_get_user_data(resource));
  if (client == nullptr || client->blocked || client->mgr == nullptr) {
    return;
  }

  for (const auto &[out_res, matrix] : client->pending) {
    CtmUpdate event{out_res, matrix};
    client->mgr->ctm_changed.emit(event);
  }
}

void ctm_destroy(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}

constexpr struct hyprland_ctm_control_manager_v1_interface ctm_manager_impl = {
    .set_ctm_for_output = ctm_set_ctm_for_output,
    .commit = ctm_commit,
    .destroy = ctm_destroy,
};

void ctm_bind(wl_client *client, void *data, uint32_t version, uint32_t id) {
  auto *mgr = static_cast<CtmControlManager::Impl *>(data);
  wl_resource *resource =
      wl_resource_create(client, &hyprland_ctm_control_manager_v1_interface,
                         static_cast<int>(version), id);
  if (resource == nullptr) {
    wl_client_post_no_memory(client);
    return;
  }
  if (mgr == nullptr) {
    wl_resource_set_implementation(resource, &ctm_manager_impl, nullptr,
                                   nullptr);
    return;
  }

  const bool blocked = (mgr->active_client != nullptr);
  auto ctm_client = std::make_unique<CtmClient>(mgr, resource, blocked);
  CtmClient *raw = ctm_client.release();
  if (!blocked) {
    mgr->active_client = raw;
  }
  mgr->all_clients.push_back(raw);

  wl_resource_set_implementation(
      resource, &ctm_manager_impl, raw, [](wl_resource *r) {
        delete static_cast<CtmClient *>(wl_resource_get_user_data(r));
      });

  if (blocked &&
      version >= HYPRLAND_CTM_CONTROL_MANAGER_V1_BLOCKED_SINCE_VERSION) {
    hyprland_ctm_control_manager_v1_send_blocked(resource);
  }
}

} // namespace

CtmControlManager::CtmControlManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
CtmControlManager::~CtmControlManager() = default;
CtmControlManager::CtmControlManager(CtmControlManager &&) noexcept = default;
CtmControlManager &
CtmControlManager::operator=(CtmControlManager &&) noexcept = default;

Signal<CtmUpdate> &CtmControlManager::ctm_changed() noexcept {
  return impl_->ctm_changed;
}

Result<CtmControlManager> CtmControlManager::create(Display &display) {
  auto impl = std::make_unique<Impl>();
  auto global =
      create_wl_global<&hyprland_ctm_control_manager_v1_interface, ctm_bind>(
          display, 2, impl.get());
  if (!global) {
    return fail(std::move(global.error().message));
  }
  impl->global = std::move(*global);
  return CtmControlManager{std::move(impl)};
}

} // namespace luminaria
