// Test virtual keyboard and virtual pointer protocols.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-server-core.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

import luminaria;
import luminaria.desktop;
import std;

namespace {

struct ClientState {
  wl_seat *seat = nullptr;
  zwp_virtual_keyboard_manager_v1 *vk_mgr = nullptr;
  zwlr_virtual_pointer_manager_v1 *vp_mgr = nullptr;

  zwp_virtual_keyboard_v1 *vk = nullptr;
  zwlr_virtual_pointer_v1 *vp = nullptr;
};

void registry_global(void *data, wl_registry *reg, uint32_t name,
                     const char *iface, uint32_t) {
  auto *st = static_cast<ClientState *>(data);
  if (std::strcmp(iface, "wl_seat") == 0) {
    st->seat = static_cast<wl_seat *>(
        wl_registry_bind(reg, name, &wl_seat_interface, 5));
  } else if (std::strcmp(iface, "zwp_virtual_keyboard_manager_v1") == 0) {
    st->vk_mgr =
        static_cast<zwp_virtual_keyboard_manager_v1 *>(wl_registry_bind(
            reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
  } else if (std::strcmp(iface, "zwlr_virtual_pointer_manager_v1") == 0) {
    st->vp_mgr =
        static_cast<zwlr_virtual_pointer_manager_v1 *>(wl_registry_bind(
            reg, name, &zwlr_virtual_pointer_manager_v1_interface, 2));
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
  assert(st.seat && st.vk_mgr && st.vp_mgr);

  st.vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(st.vk_mgr,
                                                                  st.seat);
  st.vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(st.vp_mgr,
                                                                 st.seat);
  wl_display_roundtrip(display);

  // Send virtual keyboard events
  zwp_virtual_keyboard_v1_key(st.vk, 100, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
  zwp_virtual_keyboard_v1_modifiers(st.vk, 4, 0, 0, 0);

  // Send virtual pointer events
  zwlr_virtual_pointer_v1_motion(st.vp, 110, wl_fixed_from_double(12.5),
                                 wl_fixed_from_double(-7.5));
  zwlr_virtual_pointer_v1_button(st.vp, 120, 272,
                                 WL_POINTER_BUTTON_STATE_PRESSED);
  zwlr_virtual_pointer_v1_axis(st.vp, 130, WL_POINTER_AXIS_VERTICAL_SCROLL,
                               wl_fixed_from_double(15.0));
  zwlr_virtual_pointer_v1_frame(st.vp);

  wl_display_roundtrip(display);

  zwp_virtual_keyboard_v1_destroy(st.vk);
  zwlr_virtual_pointer_v1_destroy(st.vp);
  zwp_virtual_keyboard_manager_v1_destroy(st.vk_mgr);
  zwlr_virtual_pointer_manager_v1_destroy(st.vp_mgr);
  wl_seat_destroy(st.seat);
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

  auto seat = luminaria::Seat::create(*display, "seat0");
  assert(seat.has_value());

  auto vk_mgr = luminaria::VirtualKeyboardManager::create(*display);
  assert(vk_mgr.has_value());

  auto vp_mgr = luminaria::VirtualPointerManager::create(*display);
  assert(vp_mgr.has_value());

  bool key_received = false;
  bool mod_received = false;
  bool motion_received = false;
  bool button_received = false;
  bool axis_received = false;
  bool frame_received = false;

  auto conn_key = vk_mgr->key_event().connect([&](luminaria::VirtualKey &e) {
    assert(e.time == 100);
    assert(e.key == 30);
    assert(e.pressed);
    key_received = true;
  });

  auto conn_mod =
      vk_mgr->modifiers_event().connect([&](luminaria::VirtualModifiers &e) {
        assert(e.depressed == 4);
        mod_received = true;
      });

  auto conn_motion =
      vp_mgr->motion_event().connect([&](luminaria::VirtualPointerMotion &e) {
        assert(e.time == 110);
        assert(std::abs(e.dx - 12.5) < 0.01);
        assert(std::abs(e.dy - (-7.5)) < 0.01);
        motion_received = true;
      });

  auto conn_btn =
      vp_mgr->button_event().connect([&](luminaria::VirtualPointerButton &e) {
        assert(e.time == 120);
        assert(e.button == 272);
        assert(e.pressed);
        button_received = true;
      });

  auto conn_axis =
      vp_mgr->axis_event().connect([&](luminaria::VirtualPointerAxis &e) {
        assert(e.time == 130);
        assert(e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL);
        assert(std::abs(e.value - 15.0) < 0.01);
        axis_received = true;
      });

  auto conn_frame = vp_mgr->frame_event().connect(
      [&](luminaria::VirtualPointerFrame &) { frame_received = true; });

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

  assert(key_received);
  assert(mod_received);
  assert(motion_received);
  assert(button_received);
  assert(axis_received);
  assert(frame_received);
  return 0;
}
