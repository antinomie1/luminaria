// Single-window capture: ext-foreign-toplevel-list-v1 names a window,
// ext-foreign-toplevel-image-capture-source-v1 turns the name into a capture
// source, and ext-image-copy-capture-v1 reads that window — not the screen —
// into a client buffer. This is what Zoom, Teams, Discord and the browsers do
// when the user picks "share a window".
//
// One client plays both parts, the application and the recorder. The window
// is red, then green, then bigger, and the recorder must see exactly that:
//
//   * the list announces the window with a stable 32-character identifier;
//   * a first capture delivers at once and shows the window's own pixels;
//   * a second capture while nothing changed is HELD BACK — no `ready`, no
//     `failed` — which is what keeps a shared idle window at zero wakeups;
//   * the window's next commit releases it with the new pixels;
//   * a resize fails a capture against the old size with BUFFER_CONSTRAINTS and
//     re-advertises the new size on the session;
//   * destroying the window fails the held capture with STOPPED and stops the
//     session.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

import luminaria.desktop;
import luminaria.render;
import std;

namespace {

struct Buf {
    wl_buffer* buffer = nullptr;
    std::uint8_t* pixels = nullptr;
    int w = 0, h = 0;
};

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    ext_foreign_toplevel_list_v1* list = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* source_mgr = nullptr;
    ext_image_copy_capture_manager_v1* copy_mgr = nullptr;
    wl_surface* surface = nullptr;

    // the list
    ext_foreign_toplevel_handle_v1* handle = nullptr;
    std::string identifier, title, app_id;
    int handle_dones = 0;

    // the session
    int buffer_w = 0, buffer_h = 0;
    int session_dones = 0;
    bool stopped = false;

    // the current frame
    bool ready = false;
    bool failed = false;
    std::uint32_t failure = 99;
    bool configured = false;
};

ClientState g_c;

Buf make_buffer(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const int stride = w * 4;
    const int size = stride * h;
    int fd = memfd_create("luminaria-test", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, size) == 0);
    auto* px = static_cast<std::uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(px != MAP_FAILED);
    for (int i = 0; i < w * h; ++i) { // ARGB8888, little-endian: B G R A
        px[i * 4 + 0] = b;
        px[i * 4 + 1] = g;
        px[i * 4 + 2] = r;
        px[i * 4 + 3] = 255;
    }
    wl_shm_pool* pool = wl_shm_create_pool(g_c.shm, fd, size);
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return Buf{buffer, px, w, h};
}

void xdg_surface_configure(void*, xdg_surface* xsurf, uint32_t serial) {
    xdg_surface_ack_configure(xsurf, serial);
    g_c.configured = true;
}
const xdg_surface_listener kXdgSurfaceListener{xdg_surface_configure};
void toplevel_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
void toplevel_close(void*, xdg_toplevel*) {}
void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener kToplevelListener{toplevel_configure, toplevel_close,
                                              toplevel_configure_bounds, toplevel_wm_capabilities};
void wm_base_ping(void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }
const xdg_wm_base_listener kWmBaseListener{wm_base_ping};

// ---- the list ----
void h_closed(void*, ext_foreign_toplevel_handle_v1*) {}
void h_done(void*, ext_foreign_toplevel_handle_v1*) { ++g_c.handle_dones; }
void h_title(void*, ext_foreign_toplevel_handle_v1*, const char* t) { g_c.title = t; }
void h_app_id(void*, ext_foreign_toplevel_handle_v1*, const char* a) { g_c.app_id = a; }
void h_identifier(void*, ext_foreign_toplevel_handle_v1*, const char* i) { g_c.identifier = i; }
const ext_foreign_toplevel_handle_v1_listener kHandleListener{h_closed, h_done, h_title, h_app_id,
                                                              h_identifier};
void list_toplevel(void*, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* handle) {
    g_c.handle = handle;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &kHandleListener, nullptr);
}
void list_finished(void*, ext_foreign_toplevel_list_v1*) {}
const ext_foreign_toplevel_list_v1_listener kListListener{list_toplevel, list_finished};

// ---- the capture session ----
void sess_buffer_size(void*, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
    g_c.buffer_w = static_cast<int>(w);
    g_c.buffer_h = static_cast<int>(h);
}
void sess_shm_format(void*, ext_image_copy_capture_session_v1*, uint32_t) {}
void sess_dmabuf_device(void*, ext_image_copy_capture_session_v1*, wl_array*) {}
void sess_dmabuf_format(void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {}
void sess_done(void*, ext_image_copy_capture_session_v1*) { ++g_c.session_dones; }
void sess_stopped(void*, ext_image_copy_capture_session_v1*) { g_c.stopped = true; }
const ext_image_copy_capture_session_v1_listener kSessionListener{
    sess_buffer_size, sess_shm_format, sess_dmabuf_device,
    sess_dmabuf_format, sess_done, sess_stopped};

void fr_transform(void*, ext_image_copy_capture_frame_v1*, uint32_t) {}
void fr_damage(void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {}
void fr_presentation_time(void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {}
void fr_ready(void*, ext_image_copy_capture_frame_v1*) { g_c.ready = true; }
void fr_failed(void*, ext_image_copy_capture_frame_v1*, uint32_t reason) {
    g_c.failed = true;
    g_c.failure = reason;
}
const ext_image_copy_capture_frame_v1_listener kFrameListener{
    fr_transform, fr_damage, fr_presentation_time, fr_ready, fr_failed};

void registry_global(void*, wl_registry* reg, uint32_t name, const char* iface, uint32_t) {
    if (std::strcmp(iface, "wl_compositor") == 0) {
        g_c.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(iface, "wl_shm") == 0) {
        g_c.shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (std::strcmp(iface, "xdg_wm_base") == 0) {
        g_c.wm_base = static_cast<xdg_wm_base*>(wl_registry_bind(reg, name, &xdg_wm_base_interface, 5));
    } else if (std::strcmp(iface, "ext_foreign_toplevel_list_v1") == 0) {
        g_c.list = static_cast<ext_foreign_toplevel_list_v1*>(
            wl_registry_bind(reg, name, &ext_foreign_toplevel_list_v1_interface, 1));
        ext_foreign_toplevel_list_v1_add_listener(g_c.list, &kListListener, nullptr);
    } else if (std::strcmp(iface, "ext_foreign_toplevel_image_capture_source_manager_v1") == 0) {
        g_c.source_mgr = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
            wl_registry_bind(reg, name,
                             &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1));
    } else if (std::strcmp(iface, "ext_image_copy_capture_manager_v1") == 0) {
        g_c.copy_mgr = static_cast<ext_image_copy_capture_manager_v1*>(
            wl_registry_bind(reg, name, &ext_image_copy_capture_manager_v1_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

// Outcomes the server-side main() reads back.
struct Result {
    bool first_ready = false;
    std::array<std::uint8_t, 4> first_px{};
    bool held_back = false; // second capture: neither ready nor failed
    bool second_ready = false;
    std::array<std::uint8_t, 4> second_px{};
    bool resize_failed = false;
    std::uint32_t resize_reason = 99;
    int resized_w = 0, resized_h = 0;
    bool third_ready = false;
    bool stop_failed = false;
    std::uint32_t stop_reason = 99;
    bool session_stopped = false;
    bool finished = false;
} g_r;

void roundtrips(wl_display* d, int n = 3) {
    for (int i = 0; i < n; ++i) {
        wl_display_roundtrip(d);
    }
}

/// Capture one frame into a fresh buffer of the session's current size.
ext_image_copy_capture_frame_v1* capture(ext_image_copy_capture_session_v1* session, Buf& out,
                                         int w, int h) {
    g_c.ready = g_c.failed = false;
    out = make_buffer(w, h, 0, 0, 0);
    auto* frame = ext_image_copy_capture_session_v1_create_frame(session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &kFrameListener, nullptr);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, out.buffer);
    ext_image_copy_capture_frame_v1_capture(frame);
    return frame;
}

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistry, nullptr);
    wl_display_roundtrip(display);
    assert(g_c.compositor && g_c.shm && g_c.wm_base && g_c.list && g_c.source_mgr && g_c.copy_mgr);
    xdg_wm_base_add_listener(g_c.wm_base, &kWmBaseListener, nullptr);

    // ---- the application: a red 64x48 window
    g_c.surface = wl_compositor_create_surface(g_c.compositor);
    xdg_surface* xsurf = xdg_wm_base_get_xdg_surface(g_c.wm_base, g_c.surface);
    xdg_surface_add_listener(xsurf, &kXdgSurfaceListener, nullptr);
    xdg_toplevel* toplevel = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(toplevel, &kToplevelListener, nullptr);
    xdg_toplevel_set_title(toplevel, "Slides");
    xdg_toplevel_set_app_id(toplevel, "org.luminaria.slides");
    wl_surface_commit(g_c.surface);
    roundtrips(display, 2);
    assert(g_c.configured);
    Buf red = make_buffer(64, 48, 255, 0, 0);
    wl_surface_attach(g_c.surface, red.buffer, 0, 0);
    wl_surface_damage_buffer(g_c.surface, 0, 0, 64, 48);
    wl_surface_commit(g_c.surface);
    roundtrips(display, 2);

    // ---- the recorder: list, pick the window, capture it
    roundtrips(display, 2);
    assert(g_c.handle != nullptr && g_c.handle_dones >= 1);

    ext_image_capture_source_v1* source =
        ext_foreign_toplevel_image_capture_source_manager_v1_create_source(g_c.source_mgr, g_c.handle);
    ext_image_copy_capture_session_v1* session =
        ext_image_copy_capture_manager_v1_create_session(g_c.copy_mgr, source, 0);
    ext_image_copy_capture_session_v1_add_listener(session, &kSessionListener, nullptr);
    roundtrips(display, 2);
    assert(g_c.buffer_w == 64 && g_c.buffer_h == 48); // the WINDOW's size, not the screen's
    assert(g_c.session_dones == 1 && !g_c.stopped);

    // First capture: nothing delivered yet, so it is immediate.
    Buf b1;
    auto* f1 = capture(session, b1, 64, 48);
    roundtrips(display, 1);
    g_r.first_ready = g_c.ready && !g_c.failed;
    std::memcpy(g_r.first_px.data(), b1.pixels, 4);
    ext_image_copy_capture_frame_v1_destroy(f1);

    // Second capture, window unchanged: held back.
    Buf b2;
    auto* f2 = capture(session, b2, 64, 48);
    roundtrips(display, 2);
    g_r.held_back = !g_c.ready && !g_c.failed;

    // The window commits green: the held capture is released with the new pixels.
    Buf green = make_buffer(64, 48, 0, 255, 0);
    wl_surface_attach(g_c.surface, green.buffer, 0, 0);
    wl_surface_damage_buffer(g_c.surface, 0, 0, 64, 48);
    wl_surface_commit(g_c.surface);
    roundtrips(display, 3);
    g_r.second_ready = g_c.ready && !g_c.failed;
    std::memcpy(g_r.second_px.data(), b2.pixels, 4);
    ext_image_copy_capture_frame_v1_destroy(f2);

    // The window grows to 80x60. A capture into the old 64x48 buffer fails with
    // BUFFER_CONSTRAINTS and the session re-advertises.
    Buf big = make_buffer(80, 60, 0, 0, 255);
    wl_surface_attach(g_c.surface, big.buffer, 0, 0);
    wl_surface_damage_buffer(g_c.surface, 0, 0, 80, 60);
    wl_surface_commit(g_c.surface);
    roundtrips(display, 2);
    Buf b3;
    auto* f3 = capture(session, b3, 64, 48);
    roundtrips(display, 2);
    g_r.resize_failed = g_c.failed;
    g_r.resize_reason = g_c.failure;
    g_r.resized_w = g_c.buffer_w;
    g_r.resized_h = g_c.buffer_h;
    ext_image_copy_capture_frame_v1_destroy(f3);

    // Reallocated to the new size, it works.
    Buf b4;
    auto* f4 = capture(session, b4, 80, 60);
    roundtrips(display, 2);
    g_r.third_ready = g_c.ready && !g_c.failed;
    ext_image_copy_capture_frame_v1_destroy(f4);

    // One more, held back — and then the window goes away.
    Buf b5;
    auto* f5 = capture(session, b5, 80, 60);
    roundtrips(display, 2);
    assert(!g_c.ready && !g_c.failed);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xsurf);
    wl_surface_destroy(g_c.surface);
    roundtrips(display, 3);
    g_r.stop_failed = g_c.failed;
    g_r.stop_reason = g_c.failure;
    g_r.session_stopped = g_c.stopped;
    ext_image_copy_capture_frame_v1_destroy(f5);

    g_r.finished = true;
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
}

struct DestroyCtx {
    wl_listener listener;
    luminaria::Display* display;
};
void on_client_destroy(wl_listener* l, void*) {
    auto* ctx =
        reinterpret_cast<DestroyCtx*>(reinterpret_cast<char*>(l) - offsetof(DestroyCtx, listener));
    ctx->display->terminate();
}

} // namespace

int main() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    assert(display->init_shm().has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto shell = luminaria::XdgShell::create(*display);
    assert(shell.has_value());
    auto list = luminaria::ForeignToplevelList::create(*display);
    assert(list.has_value());
    list->track(*shell);
    auto capture_mgr = luminaria::ScreencopyManager::create(*display);
    assert(capture_mgr.has_value());
    assert(capture_mgr->enable_toplevel_capture(*display, [](wl_resource* handle) {
        return luminaria::ForeignToplevelList::toplevel_for(handle);
    }).has_value());

    std::string server_identifier;
    std::size_t announced = 0;
    // The list's own map handler was connected first (track() ran before this),
    // so by the time ours runs the window is announced and has an identifier.
    luminaria::Signal<luminaria::ToplevelMap>::Connection on_map;
    auto conn = shell->new_toplevel().connect([&](luminaria::NewToplevel& e) {
        luminaria::Toplevel* tl = &e.toplevel;
        on_map = tl->map.connect([&, tl](luminaria::ToplevelMap&) {
            announced = list->toplevel_count();
            server_identifier = list->identifier(*tl);
        });
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_client, fds[1]);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(5000);

    display->run();
    client_thread.join();

    assert(g_r.finished);
    // The list.
    assert(g_c.title == "Slides");
    assert(g_c.app_id == "org.luminaria.slides");
    assert(g_c.identifier.size() == 32);
    // The pixels: ARGB8888 little-endian is B, G, R, A.
    assert(g_r.first_ready);
    assert(g_r.first_px[2] == 255 && g_r.first_px[1] == 0 && g_r.first_px[0] == 0 &&
           g_r.first_px[3] == 255);
    assert(g_r.held_back);
    assert(g_r.second_ready);
    assert(g_r.second_px[2] == 0 && g_r.second_px[1] == 255 && g_r.second_px[0] == 0);
    // Resize.
    assert(g_r.resize_failed);
    assert(g_r.resize_reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
    assert(g_r.resized_w == 80 && g_r.resized_h == 60);
    assert(g_r.third_ready);
    // Window destroyed.
    assert(g_r.stop_failed);
    assert(g_r.stop_reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
    assert(g_r.session_stopped);
    assert(announced == 1);
    assert(server_identifier == g_c.identifier);
    return 0;
}
