// A window capture source outlives the manager that made it.
//
// `ext_image_capture_source_v1` is a CLIENT object. Its teardown runs from
// libwayland — either when the client disconnects or, at the latest, inside
// `wl_display_destroy` — which is after the `ScreencopyManager` that published
// the global is already gone. That was true for the CPU path too; the GPU path
// makes it sharper, because a window source now caches Vulkan objects
// (`OffscreenTarget`, one `GpuTexture` per surface) built with a renderer the
// manager was merely lent. Teardown that walks into a freed manager, or that
// lets the renderer die while those textures are still cached, is a
// use-after-free at shutdown — when nobody is looking.
//
// So this test reaches that state on purpose: it captures a window through the
// GPU path (populating the caches), tears the manager down while the client is
// still connected and still holds its source, and only then lets the client go.
// The renderer is declared before the manager, which is the order the library
// requires — the manager (and everything it owns) dies first, which is the
// window `~ScreencopyManager::Impl` has to survive.
//
// Run under ASan: without one, a use-after-free here is silent, and "it did not
// crash" is the most this can assert.
//
// Skips (77) without a Vulkan device with dmabuf support.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
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

constexpr int kWinW = 32;
constexpr int kWinH = 24;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    ext_foreign_toplevel_list_v1* list = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* source_mgr = nullptr;
    ext_image_copy_capture_manager_v1* copy_mgr = nullptr;
    ext_foreign_toplevel_handle_v1* handle = nullptr;
    bool configured = false;
};

ClientState g_c;
/// Did the capture actually deliver? The caches this test is about are only
/// populated by a capture that ran.
bool g_ready = false;
/// Set once the capture is done: the test thread may now destroy the manager.
std::atomic<bool> g_captured{false};
/// The client only exits once the manager is gone, so the teardown order under
/// test really happens — not the client-disconnect order that hides it.
std::atomic<bool> g_manager_gone{false};

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
void h_done(void*, ext_foreign_toplevel_handle_v1*) {}
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

void sess_buffer_size(void*, ext_image_copy_capture_session_v1*, uint32_t, uint32_t) {}
void sess_shm_format(void*, ext_image_copy_capture_session_v1*, uint32_t) {}
void sess_dmabuf_device(void*, ext_image_copy_capture_session_v1*, wl_array*) {}
void sess_dmabuf_format(void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {}
void sess_done(void*, ext_image_copy_capture_session_v1*) {}
void sess_stopped(void*, ext_image_copy_capture_session_v1*) {}
const ext_image_copy_capture_session_v1_listener kSessionListener{
    sess_buffer_size, sess_shm_format, sess_dmabuf_device, sess_dmabuf_format, sess_done,
    sess_stopped};

void fr_transform(void*, ext_image_copy_capture_frame_v1*, uint32_t) {}
void fr_damage(void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {}
void fr_presentation_time(void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {}
void fr_ready(void*, ext_image_copy_capture_frame_v1*) { g_ready = true; }
void fr_failed(void*, ext_image_copy_capture_frame_v1*, uint32_t) {}
const ext_image_copy_capture_frame_v1_listener kFrameListener{fr_transform, fr_damage,
                                                             fr_presentation_time, fr_ready,
                                                             fr_failed};

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

wl_buffer* make_buffer() {
    const int stride = kWinW * 4;
    const int size = stride * kWinH;
    int fd = memfd_create("luminaria-teardown-gpu", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, size) == 0);
    auto* px = static_cast<std::uint8_t*>(
        mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(px != MAP_FAILED);
    std::memset(px, 0xFF, size);
    wl_shm_pool* pool = wl_shm_create_pool(g_c.shm, fd, size);
    wl_buffer* buffer =
        wl_shm_pool_create_buffer(pool, 0, kWinW, kWinH, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

void roundtrips(wl_display* d, int n = 3) {
    for (int i = 0; i < n; ++i) {
        wl_display_roundtrip(d);
    }
}

/// The client half. Deliberately does NOT destroy its capture objects: they are
/// the thing under test, and a real client leaks them just as carelessly when
/// the compositor exits underneath it.
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

    wl_surface* surface = wl_compositor_create_surface(g_c.compositor);
    xdg_surface* xsurf = xdg_wm_base_get_xdg_surface(g_c.wm_base, surface);
    xdg_surface_add_listener(xsurf, &kXdgSurfaceListener, nullptr);
    xdg_toplevel* toplevel = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(toplevel, &kToplevelListener, nullptr);
    xdg_toplevel_set_title(toplevel, "Teardown");
    wl_surface_commit(surface);
    roundtrips(display, 2);
    assert(g_c.configured);

    wl_surface_attach(surface, make_buffer(), 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, kWinW, kWinH);
    wl_surface_commit(surface);
    roundtrips(display, 2);
    assert(g_c.handle != nullptr);

    // The capture objects, and one capture through the GPU path so the window
    // source caches an OffscreenTarget and a texture per surface.
    ext_image_capture_source_v1* source =
        ext_foreign_toplevel_image_capture_source_manager_v1_create_source(g_c.source_mgr,
                                                                          g_c.handle);
    ext_image_copy_capture_session_v1* session =
        ext_image_copy_capture_manager_v1_create_session(g_c.copy_mgr, source, 0);
    ext_image_copy_capture_session_v1_add_listener(session, &kSessionListener, nullptr);
    roundtrips(display, 2);

    const int stride = kWinW * 4;
    const int size = stride * kWinH;
    int memfd = memfd_create("luminaria-teardown-target", MFD_CLOEXEC);
    assert(memfd >= 0);
    assert(ftruncate(memfd, size) == 0);
    wl_shm_pool* pool = wl_shm_create_pool(g_c.shm, memfd, size);
    wl_buffer* target =
        wl_shm_pool_create_buffer(pool, 0, kWinW, kWinH, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(memfd);

    auto* frame = ext_image_copy_capture_session_v1_create_frame(session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &kFrameListener, nullptr);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, target);
    ext_image_copy_capture_frame_v1_capture(frame);
    roundtrips(display, 3); // the GPU path runs here: offscreen + readback
    assert(g_ready);

    // The capture is done and the caches are populated. Wait here while the
    // server destroys the manager and the renderer underneath us: that is the
    // ordering a real exit produces, and the one the source's teardown has to
    // survive.
    g_captured.store(true, std::memory_order_release);
    // Not a roundtrip: the server has already left its loop, so anything that
    // waited on it would wait forever. Sleep instead, and only flush.
    while (!g_manager_gone.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Deliberately NOT disconnected. The client stays connected so its source,
    // session and frame are destroyed by `wl_display_destroy` — after the
    // manager, after the renderer, at the very end. That is the ordering this
    // test exists to survive, and the reason it cannot simply disconnect here.
    wl_display_flush(display);
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
    // Declared before the manager, and that order is the contract: the renderer
    // must outlive everything that caches its textures, so the manager goes
    // first and hands them back.
    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77;
    }
    if (!renderer->dmabuf_supported()) {
        std::fprintf(stderr, "skip: GPU has no dmabuf import/export\n");
        return 77;
    }

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    DestroyCtx destroy_ctx{{}, nullptr};
    destroy_ctx.listener.notify = on_client_destroy;

    auto display = luminaria::Display::create();
    assert(display.has_value());
    assert(display->init_shm().has_value());
    destroy_ctx.display = &*display;
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto shell = luminaria::XdgShell::create(*display);
    assert(shell.has_value());
    auto list = luminaria::ForeignToplevelList::create(*display);
    assert(list.has_value());
    list->track(*shell);

    // Client-side teardown runs inside wl_display_destroy, so `destroy_ctx` was
    // declared before the Display above: it has to outlive it.
    wl_client* client = nullptr;

    std::thread client_thread;
    {
        auto capture_mgr = luminaria::ScreencopyManager::create(*display);
        assert(capture_mgr.has_value());
        capture_mgr->set_renderer(&*renderer);
        assert(capture_mgr->enable_toplevel_capture(*display, [](wl_resource* handle) {
            return luminaria::ForeignToplevelList::toplevel_for(handle);
        }).has_value());

        client = wl_client_create(display->c_ptr(), fds[0]);
        assert(client != nullptr);
        wl_client_add_destroy_listener(client, &destroy_ctx.listener);

        client_thread = std::thread(run_client, fds[1]);
        // Run until the capture has been served, with a timeout so a broken
        // capture cannot hang the suite. `display->run()` is not the teardown
        // here: the client stays connected across it.
        std::atomic<bool> done{false};
        luminaria::EventSource poll;
        poll = display->event_loop().add_timer([&] {
            if (g_captured.load(std::memory_order_acquire)) {
                done.store(true, std::memory_order_release);
                display->terminate();
                return;
            }
            poll.arm(10);
        });
        auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
        timeout.arm(5000);
        poll.arm(10);
        display->run();
        assert(done.load(std::memory_order_acquire));

        // The manager dies here, while the client's window source still holds
        // the GPU objects it built. `~ScreencopyManager::Impl` is what severs
        // the back pointer and hands those objects back to the renderer.
    }
    // Churn the heap before the client's own teardown runs: a window source that
    // kept a dangling manager or renderer pointer is more likely to land on
    // reused memory than on a page that still happens to be mapped.
    {
        std::vector<void*> churn;
        for (int i = 0; i < 512; ++i) churn.push_back(std::malloc(4096));
        for (void* p : churn) std::free(p);
    }
    // Only now does the client let go, with the manager (and, above, everything
    // it owned) already destroyed. This is the ordering that makes the source's
    // own teardown the last thing to run.
    g_manager_gone.store(true, std::memory_order_release);
    client_thread.join();
    // The client is still connected: `display` is destroyed below, and its
    // `wl_display_destroy` is what finally runs the source's teardown — with the
    // manager and the renderer already gone.

    return 0;
}
