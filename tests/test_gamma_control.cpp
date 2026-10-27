// Test zwlr_gamma_control_v1: gamma size, set_gamma via fd, reset on destroy.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-server-core.h>

#include "wlr-gamma-control-unstable-v1-client-protocol.h"

import luminaria;
import luminaria.desktop;
import std;

namespace {

struct ClientState {
  wl_output *output = nullptr;
  zwlr_gamma_control_manager_v1 *manager = nullptr;
  zwlr_gamma_control_v1 *control = nullptr;
  uint32_t gamma_size = 0;
  bool failed = false;
};

void gamma_size(void *data, zwlr_gamma_control_v1 *, uint32_t size) {
  auto *st = static_cast<ClientState *>(data);
  st->gamma_size = size;
}

void gamma_failed(void *data, zwlr_gamma_control_v1 *) {
  auto *st = static_cast<ClientState *>(data);
  st->failed = true;
}

const zwlr_gamma_control_v1_listener kControlListener{gamma_size, gamma_failed};

void registry_global(void *data, wl_registry *reg, uint32_t name,
                     const char *iface, uint32_t) {
  auto *st = static_cast<ClientState *>(data);
  if (std::strcmp(iface, "wl_output") == 0) {
    st->output = static_cast<wl_output *>(
        wl_registry_bind(reg, name, &wl_output_interface, 1));
  } else if (std::strcmp(iface, "zwlr_gamma_control_manager_v1") == 0) {
    st->manager = static_cast<zwlr_gamma_control_manager_v1 *>(wl_registry_bind(
        reg, name, &zwlr_gamma_control_manager_v1_interface, 1));
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

  st.control =
      zwlr_gamma_control_manager_v1_get_gamma_control(st.manager, st.output);
  zwlr_gamma_control_v1_add_listener(st.control, &kControlListener, &st);
  wl_display_roundtrip(display);

  assert(st.gamma_size == 256);

  // Create a temporary file descriptor for gamma table
  int memfd = memfd_create("gamma", MFD_CLOEXEC);
  assert(memfd >= 0);
  const size_t total_bytes =
      static_cast<size_t>(st.gamma_size) * 3 * sizeof(uint16_t);
  assert(ftruncate(memfd, total_bytes) == 0);

  void *map =
      mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
  assert(map != MAP_FAILED);
  auto *words = static_cast<uint16_t *>(map);
  for (size_t i = 0; i < st.gamma_size; ++i) {
    words[i] = static_cast<uint16_t>(i * 256);                    // Red
    words[st.gamma_size + i] = static_cast<uint16_t>(i * 128);    // Green
    words[2 * st.gamma_size + i] = static_cast<uint16_t>(i * 64); // Blue
  }
  munmap(map, total_bytes);

  zwlr_gamma_control_v1_set_gamma(st.control, memfd);
  wl_display_roundtrip(display);

  // Destroy gamma control to test reset back to default
  zwlr_gamma_control_v1_destroy(st.control);
  wl_display_roundtrip(display);

  zwlr_gamma_control_manager_v1_destroy(st.manager);
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

  auto gamma_mgr = luminaria::GammaControlManager::create(*display, 256);
  assert(gamma_mgr.has_value());

  bool ramp_received = false;
  bool reset_received = false;
  auto conn =
      gamma_mgr->gamma_changed().connect([&](luminaria::GammaRampUpdate &e) {
        if (e.ramp.has_value()) {
          assert(e.ramp->red.size() == 256);
          assert(e.ramp->green.size() == 256);
          assert(e.ramp->blue.size() == 256);
          assert(e.ramp->red[1] == 256);
          assert(e.ramp->green[1] == 128);
          assert(e.ramp->blue[1] == 64);
          ramp_received = true;
        } else {
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

  assert(ramp_received);
  assert(reset_received);
  return 0;
}
