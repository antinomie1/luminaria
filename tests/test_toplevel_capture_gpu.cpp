// Single-window capture of a window whose buffer is a TILED dmabuf — the case
// the CPU path cannot see.
//
// A tiled dmabuf has no host mapping: its bytes are not rows of pixels, so
// `mmap` either fails or hands back a layout that is not the picture. Chrome's
// and Firefox's GPU processes hand those over for their windows. The CPU
// compositor therefore skips that layer, and a recorder sharing such a window
// gets a hole where the page content is — not an error, a transparent
// rectangle, which is the worst kind of failure because it looks like the
// window really is empty.
//
// This test builds that window and captures it twice:
//
//   1. WITHOUT `ScreencopyManager::set_renderer()` — the CPU path. The capture
//      succeeds (the window has a size and a committed buffer) but the pixels
//      are transparent: the tiled layer was skipped. This is the bug, asserted
//      deliberately; if it ever stops holding, the CPU path has learned to read
//      tiled dmabufs and the rest of this test is moot.
//   2. WITH `set_renderer()` — the GPU path composites the surface tree into an
//      OffscreenTarget and reads it back, and the client's own pixels come out.
//
// The dmabuf global is created CPU-only (`LinuxDmabuf::create(display, nullptr)`)
// so the tiled buffer really is unreadable on the CPU. The client allocates its
// modifier from the renderer's own list, passed in directly — this test is about
// the capture path, not about modifier advertisement (test_dmabuf covers that).
//
// Skips (77) without a Vulkan device, without dmabuf support, or when the
// driver offers no modifier that is not LINEAR.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

import luminaria.desktop;
import luminaria.render;
import std;

namespace {

// The window is 64x48 with two solid halves, so a capture that lost the tiled
// layer shows up as a colour that is neither.
constexpr int kWinW = 64;
constexpr int kWinH = 48;

struct Buf {
    wl_buffer* buffer = nullptr;
    int w = 0, h = 0;
};

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    xdg_wm_base* wm_base = nullptr;
    ext_foreign_toplevel_list_v1* list = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* source_mgr = nullptr;
    ext_image_copy_capture_manager_v1* copy_mgr = nullptr;
    wl_surface* surface = nullptr;

    ext_foreign_toplevel_handle_v1* handle = nullptr;
    int handle_dones = 0;

    int buffer_w = 0, buffer_h = 0;
    bool stopped = false;

    bool ready = false;
    bool failed = false;
    std::uint32_t failure = 99;
    bool configured = false;
    std::uint64_t modifier = DRM_FORMAT_MOD_INVALID;
};

ClientState g_c;

/// The modifiers the server's renderer can import for ARGB8888 — what the client
/// allocates its tiled buffer out of. Filled by main() before the client thread.
std::vector<std::uint64_t> g_renderer_modifiers;

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
void sess_done(void*, ext_image_copy_capture_session_v1*) {}
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
    } else if (std::strcmp(iface, "zwp_linux_dmabuf_v1") == 0) {
        g_c.dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 3));
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
    // The first capture, on the CPU path: it succeeds, with nothing in it.
    bool cpu_ready = false;
    bool cpu_failed = false;
    std::uint32_t cpu_failure = 99;
    int cpu_alpha = -1; // alpha of a pixel in the middle of the window
    // The second capture, on the GPU path: the client's own pixels.
    bool gpu_ready = false;
    bool gpu_failed = false;
    std::uint32_t gpu_failure = 99;
    std::array<std::uint8_t, 4> left{};  // first half of the window
    std::array<std::uint8_t, 4> right{}; // second half
    bool finished = false;
} g_r;

void roundtrips(wl_display* d, int n = 3) {
    for (int i = 0; i < n; ++i) {
        wl_display_roundtrip(d);
    }
}

/// A tiled dmabuf holding the window's picture: left half red, right half blue.
/// An empty Buf when the driver offers no non-LINEAR modifier, which is the one
/// thing that makes this test unrunnable.
Buf make_tiled_buffer(gbm_device* gbm) {
    std::uint64_t modifier = DRM_FORMAT_MOD_INVALID;
    for (std::uint64_t candidate : g_renderer_modifiers) {
        if (candidate != DRM_FORMAT_MOD_LINEAR && candidate != DRM_FORMAT_MOD_INVALID) {
            modifier = candidate;
            break;
        }
    }
    assert(modifier != DRM_FORMAT_MOD_INVALID);
    // One candidate at a time: a modifier list is a hint, and the driver is free
    // to answer a different one (or none) for a size it cannot tile.
    gbm_bo* bo = gbm_bo_create_with_modifiers2(gbm, kWinW, kWinH, GBM_FORMAT_ARGB8888, &modifier, 1,
                                               GBM_BO_USE_RENDERING);
    assert(bo != nullptr);
    const std::uint64_t actual = gbm_bo_get_modifier(bo);
    assert(actual != DRM_FORMAT_MOD_LINEAR && actual != DRM_FORMAT_MOD_INVALID);

    // Fill it through GBM, not mmap: a tiled buffer is not row-major in memory.
    std::uint32_t stride = 0;
    void* map_data = nullptr;
    void* map = gbm_bo_map(bo, 0, 0, kWinW, kWinH, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
    assert(map != nullptr && map != MAP_FAILED);
    for (int y = 0; y < kWinH; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(map) +
                                                     static_cast<std::size_t>(y) * stride);
        for (int x = 0; x < kWinW; ++x) {
            // ARGB8888: red on the left half, blue on the right.
            row[x] = x < kWinW / 2 ? 0xFFFF0000u : 0xFF0000FFu;
        }
    }
    gbm_bo_unmap(bo, map_data);

    const int fd = gbm_bo_get_fd(bo);
    const std::uint32_t offset = gbm_bo_get_offset(bo, 0);
    const std::uint32_t plane_stride = gbm_bo_get_stride(bo);
    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(g_c.dmabuf);
    zwp_linux_buffer_params_v1_add(params, fd, 0, offset, plane_stride,
                                   static_cast<std::uint32_t>(actual >> 32),
                                   static_cast<std::uint32_t>(actual));
    wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(params, kWinW, kWinH,
                                                                GBM_FORMAT_ARGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    close(fd);
    gbm_bo_destroy(bo);
    g_c.modifier = actual;
    return Buf{buffer, kWinW, kWinH};
}

/// One capture into a fresh shm buffer of the session's advertised size. Leaves
/// the pixels in `out` (mapped, still valid) and the outcome in `ready`/`failed`.
struct Capture {
    wl_buffer* buffer = nullptr;
    std::uint8_t* pixels = nullptr;
    int w = 0, h = 0, stride = 0, size = 0;
    int memfd = -1;
    bool ready = false;
    bool failed = false;
    std::uint32_t failure = 99;
};

Capture capture_once(ext_image_copy_capture_session_v1* session) {
    g_c.ready = g_c.failed = false;
    Capture cap;
    cap.w = g_c.buffer_w;
    cap.h = g_c.buffer_h;
    cap.stride = cap.w * 4;
    cap.size = cap.stride * cap.h;
    cap.memfd = memfd_create("luminaria-tiled-capture", MFD_CLOEXEC);
    assert(cap.memfd >= 0);
    assert(ftruncate(cap.memfd, cap.size) == 0);
    cap.pixels = static_cast<std::uint8_t*>(
        mmap(nullptr, cap.size, PROT_READ | PROT_WRITE, MAP_SHARED, cap.memfd, 0));
    assert(cap.pixels != MAP_FAILED);
    std::memset(cap.pixels, 0x7f, cap.size); // so "untouched" is distinguishable
    wl_shm_pool* pool = wl_shm_create_pool(g_c.shm, cap.memfd, cap.size);
    cap.buffer =
        wl_shm_pool_create_buffer(pool, 0, cap.w, cap.h, cap.stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    auto* frame = ext_image_copy_capture_session_v1_create_frame(session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &kFrameListener, nullptr);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, cap.buffer);
    ext_image_copy_capture_frame_v1_capture(frame);
    return cap;
}

void run_client(int fd, int render_fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    gbm_device* gbm = gbm_create_device(render_fd);
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistry, nullptr);
    wl_display_roundtrip(display);
    assert(g_c.compositor && g_c.shm && g_c.dmabuf && g_c.wm_base && g_c.list && g_c.source_mgr &&
           g_c.copy_mgr);
    xdg_wm_base_add_listener(g_c.wm_base, &kWmBaseListener, nullptr);

    // ---- the application: a tiled 64x48 dmabuf as its window content
    g_c.surface = wl_compositor_create_surface(g_c.compositor);
    xdg_surface* xsurf = xdg_wm_base_get_xdg_surface(g_c.wm_base, g_c.surface);
    xdg_surface_add_listener(xsurf, &kXdgSurfaceListener, nullptr);
    xdg_toplevel* toplevel = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(toplevel, &kToplevelListener, nullptr);
    xdg_toplevel_set_title(toplevel, "Tiled");
    wl_surface_commit(g_c.surface);
    roundtrips(display, 2);
    assert(g_c.configured);

    Buf tiled = make_tiled_buffer(gbm);
    wl_surface_attach(g_c.surface, tiled.buffer, 0, 0);
    wl_surface_damage_buffer(g_c.surface, 0, 0, kWinW, kWinH);
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

    // --- 1. the CPU path, before the compositor wires a renderer in
    {
        Capture cap = capture_once(session);
        roundtrips(display, 3);
        g_r.cpu_ready = g_c.ready;
        g_r.cpu_failed = g_c.failed;
        g_r.cpu_failure = g_c.failure;
        if (g_c.ready) {
            // Mid-window, away from the edges.
            g_r.cpu_alpha =
                cap.pixels[static_cast<std::size_t>(cap.h / 2) * cap.stride + (cap.w / 2) * 4 + 3];
        }
        munmap(cap.pixels, cap.size);
        close(cap.memfd);
        wl_buffer_destroy(cap.buffer);
    }

    // The compositor now hands the capture path its GPU. The client asks for it
    // the way a real one would notice a compositor doing anything — by changing
    // its title, which the server-side identity handler below turns into the
    // `set_renderer()` call. Everything else stays as it is: same session, same
    // window, same buffer.
    xdg_toplevel_set_title(toplevel, "Tiled (gpu)");
    wl_surface_commit(g_c.surface);
    roundtrips(display, 3);

    // --- 2. the same capture on the GPU path
    {
        Capture cap = capture_once(session);
        roundtrips(display, 3);
        g_r.gpu_ready = g_c.ready;
        g_r.gpu_failed = g_c.failed;
        g_r.gpu_failure = g_c.failure;
        if (g_c.ready) {
            const auto at = [&](int x, std::array<std::uint8_t, 4>& into) {
                std::memcpy(into.data(),
                            cap.pixels + static_cast<std::size_t>(cap.h / 2) * cap.stride + x * 4,
                            4);
            };
            // A quarter and three quarters across: the two halves, well away
            // from the boundary.
            at(cap.w / 4, g_r.left);
            at(cap.w * 3 / 4, g_r.right);
        }
        munmap(cap.pixels, cap.size);
        close(cap.memfd);
        wl_buffer_destroy(cap.buffer);
    }

    wl_buffer_destroy(tiled.buffer);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xsurf);
    wl_surface_destroy(g_c.surface);

    g_r.finished = true;
    wl_display_roundtrip(display);
    if (gbm != nullptr) {
        gbm_device_destroy(gbm);
    }
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
    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77;
    }
    if (!renderer->dmabuf_supported()) {
        std::fprintf(stderr, "skip: GPU has no dmabuf import/export\n");
        return 77;
    }
    int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (render_fd < 0) {
        std::fprintf(stderr, "skip: no /dev/dri/renderD128\n");
        return 77;
    }

    g_renderer_modifiers = renderer->dmabuf_modifiers(DRM_FORMAT_ARGB8888);
    bool tiled_available = false;
    for (std::uint64_t m : g_renderer_modifiers) {
        if (m != DRM_FORMAT_MOD_LINEAR && m != DRM_FORMAT_MOD_INVALID) {
            tiled_available = true;
        }
    }
    if (!tiled_available) {
        std::fprintf(stderr, "skip: GPU offers no tiled dmabuf modifier\n");
        return 77;
    }

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
    // CPU-only dmabuf global: the tiled buffer the client hands over is one the
    // CPU path genuinely cannot read, which is the whole premise.
    auto dmabuf = luminaria::LinuxDmabuf::create(*display, nullptr);
    assert(dmabuf.has_value());
    auto capture_mgr = luminaria::ScreencopyManager::create(*display);
    assert(capture_mgr.has_value());
    assert(capture_mgr->enable_toplevel_capture(*display, [](wl_resource* handle) {
        return luminaria::ForeignToplevelList::toplevel_for(handle);
    }).has_value());

    // The client asks for the GPU path by changing its window title; the
    // handler below turns that into set_renderer(), on the display thread where
    // the manager lives. Doing it from the client thread would be a data race
    // on the manager, and the whole point is that the two captures share one
    // session across the switch.
    luminaria::Signal<luminaria::ToplevelIdentityChange>::Connection on_identity;
    auto on_toplevel = shell->new_toplevel().connect([&](luminaria::NewToplevel& e) {
        luminaria::Toplevel* tl = &e.toplevel;
        on_identity =
            tl->identity_change.connect([&](luminaria::ToplevelIdentityChange& change) {
                if (change.toplevel.title() == "Tiled (gpu)") {
                    capture_mgr->set_renderer(&*renderer);
                }
            });
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_client, fds[1], render_fd);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(5000);

    display->run();
    client_thread.join();
    close(render_fd);

    assert(g_r.finished);
    assert(g_r.advertised_w == kWinW && g_r.advertised_h == kWinH);

    // 1. The CPU path cannot see the tiled layer. The capture still succeeds —
    //    the window has a size and a committed buffer — but what comes out is a
    //    transparent rectangle where the content should be. That is the bug.
    assert(g_r.cpu_ready && !g_r.cpu_failed);
    assert(g_r.cpu_alpha == 0);

    // 2. The GPU path composites the tree offscreen and reads it back: the
    //    client's own pixels, through a modifier no CPU can map. Red is
    //    (B,G,R,A) = (0,0,255,255) and blue is (255,0,0,255) in ARGB8888 order.
    assert(g_r.gpu_ready && !g_r.gpu_failed);
    assert(g_r.left[2] == 255 && g_r.left[0] == 0 && g_r.left[1] == 0 && g_r.left[3] == 255);
    assert(g_r.right[2] == 0 && g_r.right[0] == 255 && g_r.right[1] == 0 && g_r.right[3] == 255);
    return 0;
}
