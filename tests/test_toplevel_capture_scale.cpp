// Single-window capture of a HiDPI window: the window declares
// `set_buffer_scale(2)` and hands over a buffer twice its logical size, so the
// capture has to be at the BUFFER's resolution and not the window's logical
// one. Otherwise a recorder gets an upscale of a downscale — the picture it
// asked to share, resampled twice.
//
// The proof is a 1-pixel checkerboard in the client's buffer. Captured at the
// buffer's own 128x96, adjacent output pixels come from adjacent buffer pixels
// and alternate; a capture at the logical 64x48 would sample every other texel
// and come out uniform, which is exactly the bug this guards.
//
// No GPU: this is the CPU path, and the GPU path is required to agree with it.
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

// The client's buffer: 128x96 pixels, described as a 64x48 window at scale 2.
constexpr int kBufferW = 128;
constexpr int kBufferH = 96;
// The two colours of the checkerboard, in the client's ARGB8888 byte order.
constexpr std::uint8_t kRed[3] = {0, 0, 255};   // B, G, R
constexpr std::uint8_t kBlue[3] = {255, 0, 0};  // B, G, R

struct Buf {
    wl_buffer* buffer = nullptr;
    std::uint8_t* pixels = nullptr;
};

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    ext_foreign_toplevel_list_v1* list = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* source_mgr = nullptr;
    ext_image_copy_capture_manager_v1* copy_mgr = nullptr;
    wl_surface* surface = nullptr;

    ext_foreign_toplevel_handle_v1* handle = nullptr;
    int handle_dones = 0;

    int buffer_w = 0, buffer_h = 0;
    int session_dones = 0;
    bool stopped = false;

    bool ready = false;
    bool failed = false;
    std::uint32_t failure = 99;
    bool configured = false;
};

ClientState g_c;

/// A 1-pixel checkerboard, so any resampling shows up immediately.
Buf make_checkerboard() {
    const int stride = kBufferW * 4;
    const int size = stride * kBufferH;
    int fd = memfd_create("luminaria-scale", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, size) == 0);
    auto* px = static_cast<std::uint8_t*>(
        mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(px != MAP_FAILED);
    for (int y = 0; y < kBufferH; ++y) {
        for (int x = 0; x < kBufferW; ++x) {
            const std::uint8_t* c = ((x + y) & 1) == 0 ? kRed : kBlue;
            std::uint8_t* p = px + static_cast<std::size_t>(y) * stride + x * 4;
            p[0] = c[0];
            p[1] = c[1];
            p[2] = c[2];
            p[3] = 255;
        }
    }
    wl_shm_pool* pool = wl_shm_create_pool(g_c.shm, fd, size);
    wl_buffer* buffer =
        wl_shm_pool_create_buffer(pool, 0, kBufferW, kBufferH, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return Buf{buffer, px};
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

void h_closed(void*, ext_foreign_toplevel_handle_v1*) {}
void h_done(void*, ext_foreign_toplevel_handle_v1*) { ++g_c.handle_dones; }
void h_title(void*, ext_foreign_toplevel_handle_v1*, const char*) {}
void h_app_id(void*, ext_foreign_toplevel_handle_v1*, const char*) {}
void h_identifier(void*, ext_foreign_toplevel_handle_v1*, const char*) {}
const ext_foreign_toplevel_handle_v1_listener kHandleListener{h_closed, h_done, h_title, h_app_id,
                                                              h_identifier};
void list_toplevel(void*, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* handle) {
    g_c.handle = handle;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &kHandleListener, nullptr);
}
void list_finished(void*, ext_foreign_toplevel_list_v1*) {}
const ext_foreign_toplevel_list_v1_listener kListListener{list_toplevel, list_finished};

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
    sess_buffer_size, sess_shm_format, sess_dmabuf_device, sess_dmabuf_format, sess_done,
    sess_stopped};

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
        g_c.wm_base =
            static_cast<xdg_wm_base*>(wl_registry_bind(reg, name, &xdg_wm_base_interface, 5));
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

struct Result {
    int advertised_w = 0, advertised_h = 0;
    bool ready = false;
    bool failed = false;
    std::uint32_t failure = 99;
    int captured_w = 0, captured_h = 0;
    // Two adjacent pixels of one row, and two of the row below: the pattern has
    // to alternate along both axes.
    std::array<std::uint8_t, 4> px00{};
    std::array<std::uint8_t, 4> px10{};
    std::array<std::uint8_t, 4> px01{};
    std::array<std::uint8_t, 4> px11{};
    bool finished = false;
} g_r;

void roundtrips(wl_display* d, int n = 3) {
    for (int i = 0; i < n; ++i) {
        wl_display_roundtrip(d);
    }
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

    // ---- the application: a 64x48 logical window drawn into a 128x96 buffer
    g_c.surface = wl_compositor_create_surface(g_c.compositor);
    wl_surface_set_buffer_scale(g_c.surface, 2);
    xdg_surface* xsurf = xdg_wm_base_get_xdg_surface(g_c.wm_base, g_c.surface);
    xdg_surface_add_listener(xsurf, &kXdgSurfaceListener, nullptr);
    xdg_toplevel* toplevel = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(toplevel, &kToplevelListener, nullptr);
    xdg_toplevel_set_title(toplevel, "HiDPI");
    wl_surface_commit(g_c.surface);
    roundtrips(display, 2);
    assert(g_c.configured);

    Buf board = make_checkerboard();
    wl_surface_attach(g_c.surface, board.buffer, 0, 0);
    wl_surface_damage_buffer(g_c.surface, 0, 0, kBufferW, kBufferH);
    wl_surface_commit(g_c.surface);
    roundtrips(display, 2);

    // ---- the recorder
    roundtrips(display, 2);
    assert(g_c.handle != nullptr && g_c.handle_dones >= 1);

    ext_image_capture_source_v1* source =
        ext_foreign_toplevel_image_capture_source_manager_v1_create_source(g_c.source_mgr,
                                                                          g_c.handle);
    ext_image_copy_capture_session_v1* session =
        ext_image_copy_capture_manager_v1_create_session(g_c.copy_mgr, source, 0);
    ext_image_copy_capture_session_v1_add_listener(session, &kSessionListener, nullptr);
    roundtrips(display, 2);
    g_r.advertised_w = g_c.buffer_w;
    g_r.advertised_h = g_c.buffer_h;
    assert(!g_c.stopped);

    // Capture into a buffer of exactly what the session advertised — the
    // client has no choice here, and that is the point: the size the recorder
    // is told is the size it gets.
    const int cap_w = g_c.buffer_w;
    const int cap_h = g_c.buffer_h;
    const int stride = cap_w * 4;
    const int size = stride * cap_h;
    int memfd = memfd_create("luminaria-capture", MFD_CLOEXEC);
    assert(memfd >= 0);
    assert(ftruncate(memfd, size) == 0);
    auto* out = static_cast<std::uint8_t*>(
        mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0));
    assert(out != MAP_FAILED);
    std::memset(out, 0, size);
    wl_shm_pool* pool = wl_shm_create_pool(g_c.shm, memfd, size);
    wl_buffer* target =
        wl_shm_pool_create_buffer(pool, 0, cap_w, cap_h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    auto* frame = ext_image_copy_capture_session_v1_create_frame(session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &kFrameListener, nullptr);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, target);
    ext_image_copy_capture_frame_v1_capture(frame);
    roundtrips(display, 2);

    g_r.ready = g_c.ready;
    g_r.failed = g_c.failed;
    g_r.failure = g_c.failure;
    g_r.captured_w = cap_w;
    g_r.captured_h = cap_h;
    if (g_c.ready) {
        const auto at = [&](int x, int y, std::array<std::uint8_t, 4>& into) {
            std::memcpy(into.data(), out + static_cast<std::size_t>(y) * stride + x * 4, 4);
        };
        at(0, 0, g_r.px00);
        at(1, 0, g_r.px10);
        at(0, 1, g_r.px01);
        at(1, 1, g_r.px11);
    }

    ext_image_copy_capture_frame_v1_destroy(frame);
    munmap(out, size);
    close(memfd);
    wl_buffer_destroy(target);

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
    // No set_renderer(): this is the CPU path, and it has to agree with the GPU
    // one on the size a HiDPI window is captured at.
    assert(capture_mgr->enable_toplevel_capture(*display, [](wl_resource* handle) {
        return luminaria::ForeignToplevelList::toplevel_for(handle);
    }).has_value());

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
    // The size the recorder is told is the buffer's, not the window's logical
    // one — that is what lets it allocate once and get real pixels.
    assert(g_r.advertised_w == kBufferW && g_r.advertised_h == kBufferH);
    assert(g_r.ready && !g_r.failed);
    assert(g_r.captured_w == kBufferW && g_r.captured_h == kBufferH);

    // A 1-pixel checkerboard survives only if the capture is at the buffer's own
    // resolution. Downsampled to the logical 64x48, adjacent pixels would sample
    // every other texel and come out the same colour.
    const auto is_red = [](const std::array<std::uint8_t, 4>& p) {
        return p[2] == kRed[2] && p[0] == kRed[0] && p[1] == kRed[1] && p[3] == 255;
    };
    const auto is_blue = [](const std::array<std::uint8_t, 4>& p) {
        return p[2] == kBlue[2] && p[0] == kBlue[0] && p[1] == kBlue[1] && p[3] == 255;
    };
    assert(is_red(g_r.px00));
    assert(is_blue(g_r.px10)); // adjacent pixel: the other colour
    assert(is_blue(g_r.px01));
    assert(is_red(g_r.px11));
    return 0;
}
