// Test pointer gestures: swipe, pinch, and hold.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-server-core.h>

#include "pointer-gestures-unstable-v1-client-protocol.h"

import luminaria;
import std;

namespace {

struct ClientState {
  wl_seat *seat = nullptr;
  wl_pointer *pointer = nullptr;
  wl_compositor *compositor = nullptr;
  wl_surface *surface = nullptr;
  zwp_pointer_gestures_v1 *gestures = nullptr;

  zwp_pointer_gesture_swipe_v1 *swipe = nullptr;
  zwp_pointer_gesture_pinch_v1 *pinch = nullptr;
  zwp_pointer_gesture_hold_v1 *hold = nullptr;

  bool swipe_began = false;
  bool swipe_updated = false;
  bool swipe_ended = false;

  bool pinch_began = false;
  bool pinch_updated = false;
  bool pinch_ended = false;

  bool hold_began = false;
  bool hold_ended = false;
};

void swipe_begin(void *data, zwp_pointer_gesture_swipe_v1 *, uint32_t, uint32_t,
                 wl_surface *, uint32_t fingers) {
  auto *st = static_cast<ClientState *>(data);
  assert(fingers == 3);
  st->swipe_began = true;
}
void swipe_update(void *data, zwp_pointer_gesture_swipe_v1 *, uint32_t,
                  wl_fixed_t dx, wl_fixed_t dy) {
  auto *st = static_cast<ClientState *>(data);
  assert(wl_fixed_to_double(dx) == 10.0 && wl_fixed_to_double(dy) == 5.0);
  st->swipe_updated = true;
}
void swipe_end(void *data, zwp_pointer_gesture_swipe_v1 *, uint32_t, uint32_t,
               int32_t cancelled) {
  auto *st = static_cast<ClientState *>(data);
  assert(cancelled == 0);
  st->swipe_ended = true;
}
const zwp_pointer_gesture_swipe_v1_listener kSwipeListener{
    swipe_begin, swipe_update, swipe_end};

void pinch_begin(void *data, zwp_pointer_gesture_pinch_v1 *, uint32_t, uint32_t,
                 wl_surface *, uint32_t fingers) {
  auto *st = static_cast<ClientState *>(data);
  assert(fingers == 2);
  st->pinch_began = true;
}
void pinch_update(void *data, zwp_pointer_gesture_pinch_v1 *, uint32_t,
                  wl_fixed_t, wl_fixed_t, wl_fixed_t scale, wl_fixed_t) {
  auto *st = static_cast<ClientState *>(data);
  assert(wl_fixed_to_double(scale) == 1.5);
  st->pinch_updated = true;
}
void pinch_end(void *data, zwp_pointer_gesture_pinch_v1 *, uint32_t, uint32_t,
               int32_t cancelled) {
  auto *st = static_cast<ClientState *>(data);
  assert(cancelled == 0);
  st->pinch_ended = true;
}
const zwp_pointer_gesture_pinch_v1_listener kPinchListener{
    pinch_begin, pinch_update, pinch_end};

void hold_begin(void *data, zwp_pointer_gesture_hold_v1 *, uint32_t, uint32_t,
                wl_surface *, uint32_t fingers) {
  auto *st = static_cast<ClientState *>(data);
  assert(fingers == 4);
  st->hold_began = true;
}
void hold_end(void *data, zwp_pointer_gesture_hold_v1 *, uint32_t, uint32_t,
              int32_t cancelled) {
  auto *st = static_cast<ClientState *>(data);
  assert(cancelled == 0);
  st->hold_ended = true;
}
const zwp_pointer_gesture_hold_v1_listener kHoldListener{hold_begin, hold_end};

void registry_global(void *data, wl_registry *reg, uint32_t name,
                     const char *iface, uint32_t) {
  auto *st = static_cast<ClientState *>(data);
  if (std::strcmp(iface, "wl_seat") == 0) {
    st->seat = static_cast<wl_seat *>(
        wl_registry_bind(reg, name, &wl_seat_interface, 5));
  } else if (std::strcmp(iface, "wl_compositor") == 0) {
    st->compositor = static_cast<wl_compositor *>(
        wl_registry_bind(reg, name, &wl_compositor_interface, 4));
  } else if (std::strcmp(iface, "zwp_pointer_gestures_v1") == 0) {
    st->gestures = static_cast<zwp_pointer_gestures_v1 *>(
        wl_registry_bind(reg, name, &zwp_pointer_gestures_v1_interface, 3));
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
  assert(st.seat && st.compositor && st.gestures);

  st.pointer = wl_seat_get_pointer(st.seat);
  st.surface = wl_compositor_create_surface(st.compositor);
  wl_display_roundtrip(display);

  st.swipe = zwp_pointer_gestures_v1_get_swipe_gesture(st.gestures, st.pointer);
  zwp_pointer_gesture_swipe_v1_add_listener(st.swipe, &kSwipeListener, &st);

  st.pinch = zwp_pointer_gestures_v1_get_pinch_gesture(st.gestures, st.pointer);
  zwp_pointer_gesture_pinch_v1_add_listener(st.pinch, &kPinchListener, &st);

  st.hold = zwp_pointer_gestures_v1_get_hold_gesture(st.gestures, st.pointer);
  zwp_pointer_gesture_hold_v1_add_listener(st.hold, &kHoldListener, &st);
  wl_display_roundtrip(display);

  while (!st.hold_ended) {
    if (wl_display_dispatch(display) < 0) {
      break;
    }
  }

  assert(st.swipe_began && st.swipe_updated && st.swipe_ended);
  assert(st.pinch_began && st.pinch_updated && st.pinch_ended);
  assert(st.hold_began && st.hold_ended);

  zwp_pointer_gesture_swipe_v1_destroy(st.swipe);
  zwp_pointer_gesture_pinch_v1_destroy(st.pinch);
  zwp_pointer_gesture_hold_v1_destroy(st.hold);
  zwp_pointer_gestures_v1_destroy(st.gestures);
  wl_surface_destroy(st.surface);
  wl_pointer_destroy(st.pointer);
  wl_seat_destroy(st.seat);
  wl_compositor_destroy(st.compositor);
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

  auto comp = luminaria::Compositor::create(*display);
  assert(comp.has_value());

  auto seat = luminaria::Seat::create(*display, "seat0");
  assert(seat.has_value());

  auto gestures = luminaria::PointerGestures::create(*display);
  assert(gestures.has_value());

  luminaria::Surface *client_surface = nullptr;
  auto new_surf_conn = comp->new_surface().connect(
      [&](luminaria::NewSurface &e) { client_surface = &e.surface; });

  wl_client *client = wl_client_create(display->c_ptr(), fds[0]);
  assert(client != nullptr);

  DestroyCtx destroy_ctx{{}, &*display};
  destroy_ctx.listener.notify = on_client_destroy;
  wl_client_add_destroy_listener(client, &destroy_ctx.listener);

  std::thread client_thread(run_client, fds[1]);

  auto timer = display->event_loop().add_timer([&] {
    if (client_surface != nullptr) {
      // Swipe
      gestures->send_swipe_begin(*client_surface, 1, 100, 3);
      gestures->send_swipe_update(110, 10.0, 5.0);
      gestures->send_swipe_end(2, 120, false);

      // Pinch
      gestures->send_pinch_begin(*client_surface, 3, 200, 2);
      gestures->send_pinch_update(210, 0.0, 0.0, 1.5, 0.0);
      gestures->send_pinch_end(4, 220, false);

      // Hold
      gestures->send_hold_begin(*client_surface, 5, 300, 4);
      gestures->send_hold_end(6, 310, false);
    }
  });
  timer.arm(50);

  auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
  timeout.arm(2000);

  display->run();
  client_thread.join();
  return 0;
}
