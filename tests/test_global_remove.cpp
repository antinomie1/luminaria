// Test that dynamically removing a Global during runtime does not crash clients
// or cause undefined behavior (in-flight bind race safety).
#include <cassert>
#include <cstdint>
#include <cstring>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-server-core.h>

import luminaria;
import std;

namespace {

struct ClientState {
  uint32_t output_name = 0;
  bool removed = false;
  wl_output *output = nullptr;
};

void registry_global(void *data, wl_registry *, uint32_t name,
                     const char *iface, uint32_t) {
  auto *st = static_cast<ClientState *>(data);
  if (std::strcmp(iface, "wl_output") == 0) {
    st->output_name = name;
  }
}

void registry_global_remove(void *data, wl_registry *, uint32_t name) {
  auto *st = static_cast<ClientState *>(data);
  if (st->output_name == name) {
    st->removed = true;
  }
}

const wl_registry_listener kRegistry{registry_global, registry_global_remove};

void run_client(int fd) {
  wl_display *display = wl_display_connect_to_fd(fd);
  assert(display != nullptr);
  ClientState st;
  wl_registry *reg = wl_display_get_registry(display);
  wl_registry_add_listener(reg, &kRegistry, &st);
  wl_display_roundtrip(display);
  assert(st.output_name != 0);

  // Wait until server removes the global during runtime
  while (!st.removed) {
    if (wl_display_dispatch(display) < 0) {
      break;
    }
  }
  assert(st.removed);

  // Now bind the removed global (simulating in-flight bind race)
  st.output = static_cast<wl_output *>(
      wl_registry_bind(reg, st.output_name, &wl_output_interface, 1));
  assert(st.output != nullptr);

  // The bind should not trigger a fatal protocol error
  int ret = wl_display_roundtrip(display);
  assert(ret >= 0);
  assert(wl_display_get_error(display) == 0);

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

  auto og_res =
      luminaria::OutputGlobal::create(*display, 800, 600, "test-output");
  assert(og_res.has_value());
  std::optional<luminaria::OutputGlobal> output_global{std::move(*og_res)};

  wl_client *client = wl_client_create(display->c_ptr(), fds[0]);
  assert(client != nullptr);

  DestroyCtx destroy_ctx{{}, &*display};
  destroy_ctx.listener.notify = on_client_destroy;
  wl_client_add_destroy_listener(client, &destroy_ctx.listener);

  std::thread client_thread(run_client, fds[1]);

  // Timer to dynamically remove the global at runtime after client discovers it
  auto remove_timer = display->event_loop().add_timer([&] {
    output_global.reset(); // Dynamically destroyed during runtime!
  });
  remove_timer.arm(50);

  auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
  timeout.arm(2000);

  display->run();
  client_thread.join();
  return 0;
}
