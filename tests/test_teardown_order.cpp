// The globals go before the clients do, and that has to be safe.
//
// A compositor declares its Display first, so the Display is destroyed last —
// and libwayland destroys a still-connected client's resources only from
// wl_display_destroy. Every protocol global is therefore already freed when the
// destroy handlers of its client objects finally run. Each of those objects
// used to keep a raw pointer back to its manager, and dereferenced it on the
// way out: wl-copy still serving its selection when the compositor quit was
// enough to crash it, in data_device.
//
// So this test is a client that holds one of everything that points back at a
// global — a clipboard source, offers, a data-control source owning the
// primary selection, a session lock with a lock surface, foreign-toplevel and
// workspace handles, a cursor capture session, per-surface timing objects, an
// input method grab... — and stays connected while every global is destroyed,
// in the ordinary reverse-declaration order, before the Display. Run it under
// ASan: a global that does not leave its clients' objects inert is a
// heap-use-after-free at the `display.reset()` below.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <xf86drm.h>

#include "commit-timing-v1-client-protocol.h"
#include "ext-background-effect-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"
#include "fifo-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "hyprland-ctm-control-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "pointer-gestures-unstable-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

import luminaria.shell;
import luminaria.desktop;
import luminaria.shell;
import std;

namespace {

template <class T> T must(luminaria::Result<T> r) {
  if (!r) {
    std::fprintf(stderr, "test_teardown_order: %s\n",
                 r.error().message.c_str());
    std::exit(1);
  }
  return std::move(*r);
}

// ---------------------------------------------------------------- the client

struct Global {
  std::uint32_t name = 0;
  std::uint32_t version = 0;
};

struct ClientState {
  std::map<std::string, Global> globals;
  wl_registry *registry = nullptr;
  wl_shm *shm = nullptr;
  wl_surface *surface = nullptr;
  bool buffered = false;
  bool failed = false;
};

std::atomic<bool> g_ready{
    false}; // everything is set up and the server has seen it
std::atomic<bool> g_disconnected{
    false}; // the server hung up on us, as it should

void registry_global(void *data, wl_registry *, std::uint32_t name,
                     const char *interface, std::uint32_t version) {
  static_cast<ClientState *>(data)->globals[interface] = Global{name, version};
}
void registry_global_remove(void *, wl_registry *, std::uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global,
                                             registry_global_remove};

/// Bind a global the server must be advertising, at the version this side
/// knows.
template <class T> T *bind(ClientState &st, const wl_interface &iface) {
  auto it = st.globals.find(iface.name);
  if (it == st.globals.end()) {
    std::fprintf(stderr, "test_teardown_order: %s is not advertised\n",
                 iface.name);
    st.failed = true;
    return nullptr;
  }
  const auto version =
      std::min(it->second.version, static_cast<std::uint32_t>(iface.version));
  return static_cast<T *>(
      wl_registry_bind(st.registry, it->second.name, &iface, version));
}

wl_buffer *make_buffer(wl_shm *shm, int w, int h) {
  const int stride = w * 4;
  const int size = stride * h;
  int fd = memfd_create("luminaria-teardown", MFD_CLOEXEC);
  assert(fd >= 0);
  const int truncated = ftruncate(fd, size);
  assert(truncated == 0);
  (void)truncated;
  wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  wl_buffer *buffer =
      wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  return buffer;
}

/// A render node that can do timeline syncobjs, the same one the server picks.
int open_render_node() {
  for (int i = 128; i < 128 + 16; ++i) {
    const std::string path = "/dev/dri/renderD" + std::to_string(i);
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    std::uint64_t cap = 0;
    if (drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) == 0 && cap != 0) {
      return fd;
    }
    close(fd);
  }
  return -1;
}

void xdg_surface_configure(void *data, xdg_surface *xsurf,
                           std::uint32_t serial) {
  auto *st = static_cast<ClientState *>(data);
  xdg_surface_ack_configure(xsurf, serial);
  if (!st->buffered) {
    wl_surface_attach(st->surface, make_buffer(st->shm, 32, 32), 0, 0);
    wl_surface_commit(st->surface);
    st->buffered = true;
  }
}
const xdg_surface_listener kXdgSurfaceListener{xdg_surface_configure};

void run_client(int fd) {
  wl_display *display = wl_display_connect_to_fd(fd);
  if (display == nullptr) {
    return;
  }
  ClientState st;
  st.registry = wl_display_get_registry(display);
  wl_registry_add_listener(st.registry, &kRegistryListener, &st);
  wl_display_roundtrip(display);

  auto *compositor = bind<wl_compositor>(st, wl_compositor_interface);
  st.shm = bind<wl_shm>(st, wl_shm_interface);
  auto *seat = bind<wl_seat>(st, wl_seat_interface);
  auto *output = bind<wl_output>(st, wl_output_interface);
  auto *wm_base = bind<xdg_wm_base>(st, xdg_wm_base_interface);
  if (st.failed) {
    wl_display_disconnect(display);
    return;
  }
  wl_pointer *pointer = wl_seat_get_pointer(seat);
  wl_seat_get_keyboard(seat);

  // A mapped toplevel: the server focuses it, so the clipboard and the text
  // input have somewhere to send their offers and `enter`, and the foreign
  // toplevel manager has a window to hand out a handle for.
  st.surface = wl_compositor_create_surface(compositor);
  xdg_surface *xsurf = xdg_wm_base_get_xdg_surface(wm_base, st.surface);
  xdg_surface_add_listener(xsurf, &kXdgSurfaceListener, &st);
  xdg_surface_get_toplevel(xsurf);
  wl_surface_commit(st.surface);
  wl_display_roundtrip(display); // configure -> ack + buffer
  wl_display_roundtrip(display); // the mapping commit lands; focus arrives

  // Per-surface objects, each pointing back at its manager.
  wp_commit_timing_manager_v1_get_timer(
      bind<wp_commit_timing_manager_v1>(st,
                                        wp_commit_timing_manager_v1_interface),
      st.surface);
  wp_fifo_manager_v1_get_fifo(
      bind<wp_fifo_manager_v1>(st, wp_fifo_manager_v1_interface), st.surface);
  wp_fractional_scale_manager_v1_get_fractional_scale(
      bind<wp_fractional_scale_manager_v1>(
          st, wp_fractional_scale_manager_v1_interface),
      st.surface);
  ext_background_effect_manager_v1_get_background_effect(
      bind<ext_background_effect_manager_v1>(
          st, ext_background_effect_manager_v1_interface),
      st.surface);
  zwp_idle_inhibit_manager_v1_create_inhibitor(
      bind<zwp_idle_inhibit_manager_v1>(st,
                                        zwp_idle_inhibit_manager_v1_interface),
      st.surface);
  zwp_pointer_constraints_v1_lock_pointer(
      bind<zwp_pointer_constraints_v1>(st,
                                       zwp_pointer_constraints_v1_interface),
      st.surface, pointer, nullptr,
      ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
  // A presentation feedback still pending when the compositor goes.
  wp_presentation_feedback(bind<wp_presentation>(st, wp_presentation_interface),
                           st.surface);
  // Only where the machine has a render node to import timelines into.
  int drm_fd = -1;
  if (st.globals.contains(wp_linux_drm_syncobj_manager_v1_interface.name)) {
    auto *syncobj = bind<wp_linux_drm_syncobj_manager_v1>(
        st, wp_linux_drm_syncobj_manager_v1_interface);
    wp_linux_drm_syncobj_manager_v1_get_surface(syncobj, st.surface);
    drm_fd = open_render_node();
    std::uint32_t handle = 0;
    int syncobj_fd = -1;
    if (drm_fd >= 0 && drmSyncobjCreate(drm_fd, 0, &handle) == 0 &&
        drmSyncobjHandleToFD(drm_fd, handle, &syncobj_fd) == 0) {
      wp_linux_drm_syncobj_manager_v1_import_timeline(syncobj, syncobj_fd);
      close(syncobj_fd);
    }
  }
  wl_surface_commit(st.surface);

  // Seat-level objects.
  zwp_relative_pointer_manager_v1_get_relative_pointer(
      bind<zwp_relative_pointer_manager_v1>(
          st, zwp_relative_pointer_manager_v1_interface),
      pointer);
  ext_idle_notifier_v1_get_idle_notification(
      bind<ext_idle_notifier_v1>(st, ext_idle_notifier_v1_interface), 60'000,
      seat);
  zwp_text_input_manager_v3_get_text_input(
      bind<zwp_text_input_manager_v3>(st, zwp_text_input_manager_v3_interface),
      seat);
  zwp_input_method_v2 *im = zwp_input_method_manager_v2_get_input_method(
      bind<zwp_input_method_manager_v2>(st,
                                        zwp_input_method_manager_v2_interface),
      seat);
  zwp_input_method_v2_grab_keyboard(im);

  // The clipboard: a client source owning it, an unused primary source, and
  // a clipboard manager owning the primary selection through data-control.
  auto *ddm =
      bind<wl_data_device_manager>(st, wl_data_device_manager_interface);
  wl_data_device_manager_get_data_device(ddm, seat);
  wl_data_device *data_device =
      wl_data_device_manager_get_data_device(ddm, seat);
  wl_data_source *source = wl_data_device_manager_create_data_source(ddm);
  wl_data_source_offer(source, "text/plain");
  wl_data_device_set_selection(data_device, source, 0);
  auto *psm = bind<zwp_primary_selection_device_manager_v1>(
      st, zwp_primary_selection_device_manager_v1_interface);
  zwp_primary_selection_device_manager_v1_get_device(psm, seat);
  zwp_primary_selection_source_v1_offer(
      zwp_primary_selection_device_manager_v1_create_source(psm), "text/plain");
  auto *dcm = bind<zwlr_data_control_manager_v1>(
      st, zwlr_data_control_manager_v1_interface);
  zwlr_data_control_device_v1 *control_device =
      zwlr_data_control_manager_v1_get_data_device(dcm, seat);
  zwlr_data_control_source_v1 *control_source =
      zwlr_data_control_manager_v1_create_data_source(dcm);
  zwlr_data_control_source_v1_offer(control_source, "text/plain;charset=utf-8");
  zwlr_data_control_device_v1_set_primary_selection(control_device,
                                                    control_source);
  zwlr_data_control_manager_v1_create_data_source(dcm); // never used
  wl_display_roundtrip(display); // the selections are set; offers go out
  wl_display_roundtrip(display);

  // A session lock with its lock surface.
  ext_session_lock_v1 *lock =
      ext_session_lock_manager_v1_lock(bind<ext_session_lock_manager_v1>(
          st, ext_session_lock_manager_v1_interface));
  ext_session_lock_v1_get_lock_surface(
      lock, wl_compositor_create_surface(compositor), output);

  // Handle-bearing managers: a taskbar and a workspace pager.
  bind<zwlr_foreign_toplevel_manager_v1>(
      st, zwlr_foreign_toplevel_manager_v1_interface);
  bind<ext_workspace_manager_v1>(st, ext_workspace_manager_v1_interface);

  // A pointer-cursor capture session.
  ext_image_capture_source_v1 *capture_source =
      ext_output_image_capture_source_manager_v1_create_source(
          bind<ext_output_image_capture_source_manager_v1>(
              st, ext_output_image_capture_source_manager_v1_interface),
          output);
  ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
      bind<ext_image_copy_capture_manager_v1>(
          st, ext_image_copy_capture_manager_v1_interface),
      capture_source, pointer);

  // Gestures, gamma, CTM, virtual keyboard, and virtual pointer.
  auto *pg =
      bind<zwp_pointer_gestures_v1>(st, zwp_pointer_gestures_v1_interface);
  if (pg) {
    zwp_pointer_gestures_v1_get_swipe_gesture(pg, pointer);
  }
  auto *gm = bind<zwlr_gamma_control_manager_v1>(
      st, zwlr_gamma_control_manager_v1_interface);
  if (gm) {
    zwlr_gamma_control_manager_v1_get_gamma_control(gm, output);
  }
  bind<hyprland_ctm_control_manager_v1>(
      st, hyprland_ctm_control_manager_v1_interface);
  auto *vk = bind<zwp_virtual_keyboard_manager_v1>(
      st, zwp_virtual_keyboard_manager_v1_interface);
  if (vk) {
    zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vk, seat);
  }
  auto *vp = bind<zwlr_virtual_pointer_manager_v1>(
      st, zwlr_virtual_pointer_manager_v1_interface);
  if (vp) {
    zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vp, seat);
  }

  if (st.failed || wl_display_roundtrip(display) < 0 ||
      wl_display_roundtrip(display) < 0) {
    std::fprintf(stderr,
                 "test_teardown_order: setup failed (protocol error?)\n");
    wl_display_disconnect(display);
    return;
  }
  g_ready = true;

  // Stay connected, holding all of it, until the server goes away.
  while (wl_display_dispatch(display) != -1) {
  }
  g_disconnected = true;
  wl_display_disconnect(display);
  if (drm_fd >= 0) {
    close(drm_fd);
  }
}

// ---------------------------------------------------------------- the server

struct DestroyCtx {
  wl_listener listener;
  bool destroyed = false;
};
void on_client_destroy(wl_listener *l, void *) {
  reinterpret_cast<DestroyCtx *>(reinterpret_cast<char *>(l) -
                                 offsetof(DestroyCtx, listener))
      ->destroyed = true;
}

} // namespace

int main() {
  int fds[2];
  const int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
  assert(rc == 0);
  (void)rc;

  // Declared first, destroyed last — exactly what every compositor does.
  std::optional<luminaria::Display> display =
      must(luminaria::Display::create());
  const bool shm = display->init_shm().has_value();
  assert(shm);
  (void)shm;
  DestroyCtx destroy_ctx{};
  std::thread client_thread;

  {
    auto compositor = must(luminaria::Compositor::create(*display));
    auto seat = must(luminaria::Seat::create(*display));
    auto output = must(luminaria::OutputGlobal::create(*display, 80, 60));
    auto shell = must(luminaria::XdgShell::create(*display));
    auto presentation = must(luminaria::Presentation::create(*display));
    auto commit_timing = must(luminaria::CommitTimingManager::create(*display));
    auto fifo = must(luminaria::FifoManager::create(*display));
    auto fractional = must(luminaria::FractionalScaleManager::create(*display));
    auto background =
        must(luminaria::BackgroundEffectManager::create(*display));
    auto inhibit = must(luminaria::IdleInhibitManager::create(*display));
    auto idle =
        must(luminaria::IdleNotifier::create(*display, display->event_loop()));
    auto relative =
        must(luminaria::RelativePointerManager::create(*display, seat));
    auto constraints =
        must(luminaria::PointerConstraints::create(*display, seat));
    auto text_inputs =
        must(luminaria::TextInputManager::create(*display, seat));
    auto input_methods = must(
        luminaria::InputMethodManager::create(*display, seat, text_inputs));
    auto data_device =
        must(luminaria::DataDeviceManager::create(*display, seat));
    auto primary =
        must(luminaria::PrimarySelectionManager::create(*display, seat));
    auto data_control = must(
        luminaria::DataControlManager::create(*display, data_device, &primary));
    auto lock = must(luminaria::SessionLockManager::create(*display));
    auto toplevels = must(luminaria::ForeignToplevelManager::create(*display));
    toplevels.track(shell);
    auto workspaces = must(luminaria::WorkspaceManager::create(*display));
    const std::uint32_t group = workspaces.add_group();
    (void)workspaces.add_workspace(group, "one", "One");
    workspaces.done();
    auto gestures = must(luminaria::PointerGestures::create(*display));
    auto gamma = must(luminaria::GammaControlManager::create(*display));
    auto ctm = must(luminaria::CtmControlManager::create(*display));
    auto vk_mgr = must(luminaria::VirtualKeyboardManager::create(*display));
    auto vp_mgr = must(luminaria::VirtualPointerManager::create(*display));
    auto screencopy = must(luminaria::ScreencopyManager::create(*display));
    screencopy.set_cursor_source(
        [](wl_resource *, luminaria::CursorCapture &out) {
          out.width = 1;
          out.height = 1;
          out.rgba.assign(4, 255);
          return true;
        });
    output.on_bind([&](wl_resource *res) {
      screencopy.add_output(
          res, 80, 60,
          [](int, int, int w, int h, std::vector<std::uint8_t> &rgba) {
            rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
            return true;
          });
    });
    // Optional: it needs a render node with timeline syncobjs.
    std::optional<luminaria::DrmSyncobjManager> syncobj;
    if (auto s = luminaria::DrmSyncobjManager::create(*display); s) {
      syncobj.emplace(std::move(*s));
    }

    // Focus the window as soon as it maps, so offers and `enter` go out.
    std::vector<luminaria::Signal<luminaria::ToplevelMap>::Connection>
        map_conns;
    auto new_toplevel =
        shell.new_toplevel().connect([&](luminaria::NewToplevel &e) {
          map_conns.push_back(
              e.toplevel.map.connect([&](luminaria::ToplevelMap &m) {
                seat.set_keyboard_focus(&m.toplevel.surface());
                seat.pointer_enter(m.toplevel.surface(), 1, 1);
              }));
        });

    // Grant the pointer lock, so the manager's `active` points at it too.
    auto new_constraint = constraints.new_constraint().connect(
        [](luminaria::NewPointerConstraint &e) { e.constraint.activate(); });

    wl_client *client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);
    client_thread = std::thread(run_client, fds[1]);

    // Serve the client until it says it holds everything (or gives up).
    bool timed_out = false;
    auto timeout = display->event_loop().add_timer([&] {
      timed_out = true;
      display->terminate();
    });
    timeout.arm(5000);
    luminaria::EventSource poll;
    poll = display->event_loop().add_timer([&] {
      if (g_ready || destroy_ctx.destroyed) {
        display->terminate();
        return;
      }
      poll.arm(2);
    });
    poll.arm(2);
    display->run();

    assert(!timed_out);
    assert(g_ready);
    assert(!destroy_ctx.destroyed); // still connected: that is the point

    // The setup really reached the paths this test is about.
    assert(data_device.selection_mime_types().size() == 1);
    assert(primary.selection_source() != nullptr); // the data-control source
    assert(idle.notification_count() == 1);
    assert(lock.current() != nullptr);
    assert(lock.current()->surfaces().size() == 1);
    assert(input_methods.current() != nullptr);
    assert(input_methods.current()->keyboard_grab() != nullptr);
    assert(constraints.active_constraint() != nullptr);
    (void)timed_out;
  } // every global is destroyed here, while the client still holds its objects

  assert(!destroy_ctx.destroyed);
  // Now libwayland destroys the client and runs every destroy handler, into
  // globals that no longer exist.
  display.reset();
  assert(destroy_ctx.destroyed);

  client_thread.join();
  assert(g_disconnected);
  return 0;
}
