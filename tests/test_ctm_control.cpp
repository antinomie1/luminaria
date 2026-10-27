// Test hyprland_ctm_control_v1: set_ctm_for_output, commit, reset on destroy.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-server-core.h>

#include "hyprland-ctm-control-v1-client-protocol.h"

import luminaria;
import luminaria.desktop;
import std;

namespace {

struct ClientState {
  wl_output *output = nullptr;
  hyprland_ctm_control_manager_v1 *manager = nullptr;
  bool blocked = false;
};

void ctm_blocked(void *data, hyprland_ctm_control_manager_v1 *) {
  auto *st = static_cast<ClientState *>(data);
  st->blocked = true;
}

const hyprland_ctm_control_manager_v1_listener kCtmListener{ctm_blocked};

void registry_global(void *data, wl_registry *reg, uint32_t name,
                     const char *iface, uint32_t) {
  auto *st = static_cast<ClientState *>(data);
  if (std::strcmp(iface, "wl_output") == 0) {
    st->output = static_cast<wl_output *>(
        wl_registry_bind(reg, name, &wl_output_interface, 1));
  } else if (std::strcmp(iface, "hyprland_ctm_control_manager_v1") == 0) {
    st->manager =
        static_cast<hyprland_ctm_control_manager_v1 *>(wl_registry_bind(
            reg, name, &hyprland_ctm_control_manager_v1_interface, 2));
  }
}
void registry_global_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

void run_client(int fd) {
  wl_display *display = wl_display_connect_to_fd(fd);
  assert(display != nullptr);
  ClientState st;
  wl_registry *reg = wl_display_get_registry(display);
  wl_registry_add_listener(reg, &kRegistry, &st);
  wl_display_roundtrip(display);
  assert(st.output && st.manager);

  hyprland_ctm_control_manager_v1_add_listener(st.manager, &kCtmListener, &st);
  wl_display_roundtrip(display);
  assert(!st.blocked);

  // Set CTM matrix (sepia / tint matrix)
  hyprland_ctm_control_manager_v1_set_ctm_for_output(
      st.manager, st.output, wl_fixed_from_double(1.0),
      wl_fixed_from_double(0.2), wl_fixed_from_double(0.0),
      wl_fixed_from_double(0.1), wl_fixed_from_double(0.9),
      wl_fixed_from_double(0.0), wl_fixed_from_double(0.0),
      wl_fixed_from_double(0.1), wl_fixed_from_double(0.8));

  hyprland_ctm_control_manager_v1_commit(st.manager);
  wl_display_roundtrip(display);

  // Destroy to verify reset back to identity
  hyprland_ctm_control_manager_v1_destroy(st.manager);
  wl_display_roundtrip(display);

  wl_output_destroy(st.output);
  wl_registry_destroy(reg);
  wl_display_disconnect(display);
}

struct DestroyCtx {
  wl_listener listener;
  luminaria::Display *display;
};

void on_client_destroy(wl_listener *l, void *) {
  auto *ctx = wl_container_of(l, static_cast<DestroyCtx *>(nullptr), listener);
  ctx->display->terminate();
}

} // namespace

int main() {
  int fds[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);

  auto display = luminaria::Display::create();
  assert(display.has_value());

  auto output_global =
      luminaria::OutputGlobal::create(*display, 800, 600, "HEAD-1");
  assert(output_global.has_value());

  auto ctm_mgr = luminaria::CtmControlManager::create(*display);
  assert(ctm_mgr.has_value());

  bool matrix_received = false;
  bool reset_received = false;
  auto conn = ctm_mgr->ctm_changed().connect([&](luminaria::CtmUpdate &e) {
    if (e.matrix[1] > 0.15) {
      assert(e.matrix[0] == 1.0);
      assert(std::abs(e.matrix[1] - 0.2) < 0.01);
      assert(std::abs(e.matrix[4] - 0.9) < 0.01);
      matrix_received = true;
    } else if (e.matrix == luminaria::identity_ctm()) {
      reset_received = true;
    }
  });

  wl_client *client = wl_client_create(display->c_ptr(), fds[0]);
  assert(client != nullptr);

  DestroyCtx destroy_ctx{{}, &*display};
  destroy_ctx.listener.notify = on_client_destroy;
  wl_client_add_destroy_listener(client, &destroy_ctx.listener);

  std::thread client_thread(run_client, fds[1]);

  auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
  timeout.arm(2000);

  display->run();
  client_thread.join();

  assert(matrix_received);
  assert(reset_received);
  return 0;
}
