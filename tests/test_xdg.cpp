// Phase 2 slice 2: full xdg-shell toplevel lifecycle. An in-process client binds
// xdg_wm_base, creates a toplevel, completes the configure handshake, attaches a
// real wl_shm buffer, and commits. The server asserts new_toplevel + map fired.
#include <cassert>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "xdg-shell-client-protocol.h"

import luminaria;

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_surface* surface = nullptr;
    bool committed_buffer = false;
};

wl_buffer* make_buffer(ClientState* st, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    int fd = memfd_create("luminaria-test", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, size) == 0);
    wl_shm_pool* pool = wl_shm_create_pool(st->shm, fd, size);
    wl_buffer* buffer =
        wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

void xdg_surface_configure(void* data, xdg_surface* xsurf, uint32_t serial) {
    auto* st = static_cast<ClientState*>(data);
    xdg_surface_ack_configure(xsurf, serial);
    if (!st->committed_buffer) {
        wl_buffer* buffer = make_buffer(st, 64, 64);
        wl_surface_attach(st->surface, buffer, 0, 0);
        wl_surface_commit(st->surface); // this commit MAPS the toplevel
        st->committed_buffer = true;
    }
}
const xdg_surface_listener kXdgSurfaceListener{xdg_surface_configure};

void toplevel_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
void toplevel_close(void*, xdg_toplevel*) {}
const xdg_toplevel_listener kToplevelListener{toplevel_configure, toplevel_close};

void wm_base_ping(void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }
const xdg_wm_base_listener kWmBaseListener{wm_base_ping};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t /*version*/) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_shm") == 0) {
        st->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, "xdg_wm_base") == 0) {
        st->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState st;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display); // receive globals

    if (st.compositor != nullptr && st.shm != nullptr && st.wm_base != nullptr) {
        xdg_wm_base_add_listener(st.wm_base, &kWmBaseListener, &st);
        st.surface = wl_compositor_create_surface(st.compositor);
        xdg_surface* xsurf = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
        xdg_surface_add_listener(xsurf, &kXdgSurfaceListener, &st);
        xdg_toplevel* toplevel = xdg_surface_get_toplevel(xsurf);
        xdg_toplevel_add_listener(toplevel, &kToplevelListener, &st);

        wl_surface_commit(st.surface); // initial commit -> server sends configure
        wl_display_roundtrip(display); // configure arrives; listener acks+attaches+commits
        wl_display_roundtrip(display); // ensure the mapping commit is sent AND processed
    }
    wl_display_disconnect(display); // server terminates on our disconnect
}

// Server-side: terminate the loop when the client goes away (never mid-session,
// so the client can roundtrip freely without deadlocking).
struct DestroyCtx {
    wl_listener listener;
    luminaria::Display* display;
};
void on_client_destroy(wl_listener* l, void*) {
    auto* ctx = reinterpret_cast<DestroyCtx*>(reinterpret_cast<char*>(l) -
                                              offsetof(DestroyCtx, listener));
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

    bool saw_toplevel = false;
    bool saw_map = false;
    std::vector<luminaria::Signal<luminaria::ToplevelMap>::Connection> map_conns;

    auto nt = shell->new_toplevel().connect([&](luminaria::NewToplevel& e) {
        saw_toplevel = true;
        map_conns.push_back(
            e.toplevel.map.connect([&](luminaria::ToplevelMap&) { saw_map = true; }));
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_client, fds[1]);

    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(3000);

    display->run();
    client_thread.join();

    assert(saw_toplevel);
    assert(saw_map);
    return 0;
}
