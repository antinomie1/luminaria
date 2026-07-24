// xdg_toplevel window state: title / app_id / min-max size hints reach the
// server, a maximize request is arbitrated by the compositor, and the answering
// configure carries both the new size and the MAXIMIZED state (plus
// configure_bounds and wm_capabilities from xdg_wm_base v4/v5).
#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "xdg-shell-client-protocol.h"

#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"
#include "luminaria/xdg_shell.hpp"

namespace {

constexpr int kMaxW = 800;
constexpr int kMaxH = 600;
constexpr int kBoundsW = 1280;
constexpr int kBoundsH = 720;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_surface* surface = nullptr;
    bool buffered = false;

    int last_cfg_w = -1, last_cfg_h = -1;
    bool cfg_maximized = false;
    bool cfg_activated = false;
    int bounds_w = -1, bounds_h = -1;
    bool got_wm_capabilities = false;
    bool got_close = false;
};

wl_buffer* make_buffer(ClientState* st, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    int fd = memfd_create("luminaria-test", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, size) == 0);
    wl_shm_pool* pool = wl_shm_create_pool(st->shm, fd, size);
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

void xdg_surface_configure(void* data, xdg_surface* xsurf, uint32_t serial) {
    auto* st = static_cast<ClientState*>(data);
    xdg_surface_ack_configure(xsurf, serial);
    if (!st->buffered) {
        wl_surface_attach(st->surface, make_buffer(st, 64, 64), 0, 0);
        wl_surface_commit(st->surface);
        st->buffered = true;
    }
}
const xdg_surface_listener kXdgSurfaceListener{xdg_surface_configure};

void toplevel_configure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                        wl_array* states) {
    auto* st = static_cast<ClientState*>(data);
    st->last_cfg_w = width;
    st->last_cfg_h = height;
    st->cfg_maximized = false;
    st->cfg_activated = false;
    const auto* first = static_cast<const uint32_t*>(states->data);
    const size_t count = states->size / sizeof(uint32_t);
    for (size_t i = 0; i < count; ++i) {
        if (first[i] == XDG_TOPLEVEL_STATE_MAXIMIZED) {
            st->cfg_maximized = true;
        }
        if (first[i] == XDG_TOPLEVEL_STATE_ACTIVATED) {
            st->cfg_activated = true;
        }
    }
}
void toplevel_close(void* data, xdg_toplevel*) {
    static_cast<ClientState*>(data)->got_close = true;
}
void toplevel_configure_bounds(void* data, xdg_toplevel*, int32_t width, int32_t height) {
    auto* st = static_cast<ClientState*>(data);
    st->bounds_w = width;
    st->bounds_h = height;
}
void toplevel_wm_capabilities(void* data, xdg_toplevel*, wl_array*) {
    static_cast<ClientState*>(data)->got_wm_capabilities = true;
}
const xdg_toplevel_listener kToplevelListener{toplevel_configure, toplevel_close,
                                              toplevel_configure_bounds,
                                              toplevel_wm_capabilities};

void wm_base_ping(void*, xdg_wm_base* wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}
const xdg_wm_base_listener kWmBaseListener{wm_base_ping};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_shm") == 0) {
        st->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, "xdg_wm_base") == 0) {
        st->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 5));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

ClientState g_client;

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState& st = g_client;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display);

    if (st.compositor != nullptr && st.shm != nullptr && st.wm_base != nullptr) {
        xdg_wm_base_add_listener(st.wm_base, &kWmBaseListener, &st);
        st.surface = wl_compositor_create_surface(st.compositor);
        xdg_surface* xsurf = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
        xdg_surface_add_listener(xsurf, &kXdgSurfaceListener, &st);
        xdg_toplevel* toplevel = xdg_surface_get_toplevel(xsurf);
        xdg_toplevel_add_listener(toplevel, &kToplevelListener, &st);
        xdg_toplevel_set_title(toplevel, "Luminaria Test");
        xdg_toplevel_set_app_id(toplevel, "org.luminaria.test");
        xdg_toplevel_set_min_size(toplevel, 100, 50);
        xdg_toplevel_set_max_size(toplevel, kMaxW, kMaxH);
        xdg_surface_set_window_geometry(xsurf, 4, 4, 56, 56);

        wl_surface_commit(st.surface);  // initial commit -> first configure
        wl_display_roundtrip(display);  // ack + buffer + map
        wl_display_roundtrip(display);  // the server sees the mapping commit

        xdg_toplevel_set_maximized(toplevel); // the compositor decides
        wl_display_roundtrip(display);
        wl_display_roundtrip(display); // the answering configure arrives
    }
    wl_display_disconnect(display);
}

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
    shell->set_bounds(kBoundsW, kBoundsH);

    std::string seen_title, seen_app_id;
    int seen_min_w = -1, seen_min_h = -1, seen_max_w = -1, seen_max_h = -1;
    luminaria::XdgGeometry seen_geometry{};
    bool saw_maximize_request = false;
    bool server_thinks_maximized = false;
    std::vector<luminaria::Signal<luminaria::ToplevelMap>::Connection> map_conns;
    std::vector<luminaria::Signal<luminaria::ToplevelRequestMaximize>::Connection> max_conns;

    auto nt = shell->new_toplevel().connect([&](luminaria::NewToplevel& e) {
        luminaria::Toplevel* tl = &e.toplevel;
        map_conns.push_back(tl->map.connect([&, tl](luminaria::ToplevelMap&) {
            seen_title = tl->title();
            seen_app_id = tl->app_id();
            seen_min_w = tl->min_width();
            seen_min_h = tl->min_height();
            seen_max_w = tl->max_width();
            seen_max_h = tl->max_height();
            seen_geometry = tl->geometry();
            tl->set_activated(true);
        }));
        max_conns.push_back(
            tl->request_maximize.connect([&, tl](luminaria::ToplevelRequestMaximize& ev) {
                saw_maximize_request = ev.maximized;
                // Grant it and give the client the size we want it to take.
                tl->set_maximized(ev.maximized);
                (void)tl->configure(kBoundsW, kBoundsH);
                server_thinks_maximized = tl->is_maximized();
            }));
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

    assert(seen_title == "Luminaria Test");
    assert(seen_app_id == "org.luminaria.test");
    assert(seen_min_w == 100 && seen_min_h == 50);
    assert(seen_max_w == kMaxW && seen_max_h == kMaxH);
    assert(seen_geometry.x == 4 && seen_geometry.y == 4);
    assert(seen_geometry.width == 56 && seen_geometry.height == 56);
    assert(saw_maximize_request);
    assert(server_thinks_maximized);

    assert(g_client.bounds_w == kBoundsW && g_client.bounds_h == kBoundsH);
    assert(g_client.got_wm_capabilities);
    assert(g_client.last_cfg_w == kBoundsW && g_client.last_cfg_h == kBoundsH);
    assert(g_client.cfg_maximized);
    assert(g_client.cfg_activated);
    return 0;
}
