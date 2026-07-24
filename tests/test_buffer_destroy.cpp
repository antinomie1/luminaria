// Regression: a client destroys a wl_buffer the compositor still holds.
//
// Every toolkit does this — Qt/GTK drop the whole swapchain when a window is
// resized, hidden, or re-shown. The compositor used to keep the raw
// wl_resource* and then call wl_buffer.release on it (and read pixels from it),
// which segfaulted. Surface must drop the buffer from every slot instead.
//
// Also covers the xdg-shell half of the same report: a configure that carries
// MAXIMIZED must never name a 0x0 size ("Configure event with maximized or
// fullscreen state contains invalid width: 0" in Qt's log).
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

#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"
#include "luminaria/xdg_shell.hpp"

namespace {

constexpr int kBoundsW = 1024;
constexpr int kBoundsH = 768;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_surface* surface = nullptr;
    wl_buffer* first = nullptr;
    bool buffered = false;

    int cfg_w = -1, cfg_h = -1;
    bool cfg_maximized = false;
    bool saw_invalid_maximized_configure = false;
};

ClientState g_client;

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
        st->first = make_buffer(st, 64, 64);
        wl_surface_attach(st->surface, st->first, 0, 0);
        wl_surface_commit(st->surface);
        st->buffered = true;
    }
}
const xdg_surface_listener kXdgSurfaceListener{xdg_surface_configure};

void toplevel_configure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                        wl_array* states) {
    auto* st = static_cast<ClientState*>(data);
    st->cfg_w = width;
    st->cfg_h = height;
    st->cfg_maximized = false;
    const auto* first = static_cast<const uint32_t*>(states->data);
    for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i) {
        if (first[i] == XDG_TOPLEVEL_STATE_MAXIMIZED ||
            first[i] == XDG_TOPLEVEL_STATE_FULLSCREEN) {
            st->cfg_maximized = true;
        }
    }
    // This is exactly what Qt complains about and then ignores.
    if (st->cfg_maximized && (width == 0 || height == 0)) {
        st->saw_invalid_maximized_configure = true;
    }
}
void toplevel_close(void*, xdg_toplevel*) {}
void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}
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
        wl_surface_commit(st.surface);
        wl_display_roundtrip(display); // configure -> ack -> attach -> map
        wl_display_roundtrip(display);

        // Maximize: the answering configure must carry a real size.
        xdg_toplevel_set_maximized(toplevel);
        wl_display_roundtrip(display);
        wl_display_roundtrip(display);

        // The heart of it: throw away the buffer the compositor is holding,
        // then hand it a new one. The old code released the freed resource.
        wl_buffer_destroy(st.first);
        st.first = nullptr;
        wl_display_roundtrip(display); // the server must forget it HERE

        wl_surface_attach(st.surface, make_buffer(&st, 96, 96), 0, 0);
        wl_surface_commit(st.surface);
        wl_display_roundtrip(display);
        wl_display_roundtrip(display);

        // And again the other way round: attach a buffer, destroy it before the
        // commit that would apply it, then commit.
        wl_buffer* doomed = make_buffer(&st, 32, 32);
        wl_surface_attach(st.surface, doomed, 0, 0);
        wl_buffer_destroy(doomed);
        wl_surface_commit(st.surface);
        wl_display_roundtrip(display);
        wl_display_roundtrip(display);
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

    // Read pixels back on every commit, the way a real compositor's frame loop
    // does — that is the other place a dead buffer resource used to be touched.
    int commits = 0;
    int reads_with_buffer = 0;
    bool last_commit_had_buffer = true;
    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* surface = &e.surface;
        conns.push_back(e.surface.commit.connect([&, surface](luminaria::SurfaceCommit&) {
            ++commits;
            last_commit_had_buffer = surface->has_buffer();
            std::vector<std::uint8_t> rgba;
            int w = 0, h = 0;
            if (surface->current_buffer_rgba(rgba, w, h)) {
                ++reads_with_buffer;
            }
        }));
    });

    // Grant maximize without naming a size: the shell must fill in its bounds
    // rather than emit a 0x0 configure carrying the MAXIMIZED state.
    std::vector<luminaria::Signal<luminaria::ToplevelRequestMaximize>::Connection> max_conns;
    auto nt = shell->new_toplevel().connect([&](luminaria::NewToplevel& e) {
        luminaria::Toplevel* tl = &e.toplevel;
        max_conns.push_back(tl->request_maximize.connect(
            [tl](luminaria::ToplevelRequestMaximize& ev) { tl->set_maximized(ev.maximized); }));
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

    // Surviving at all is the crash test; the counters prove we really did drive
    // commits and readbacks across the destroyed-buffer window.
    assert(commits == 4);      // initial, first buffer, replacement, doomed
    assert(reads_with_buffer == 2); // only the two commits with a live buffer
    // The last commit applied a buffer the client had already destroyed: the
    // surface must have degraded it to "no buffer", not kept a dead resource.
    assert(!last_commit_had_buffer);
    assert(!g_client.saw_invalid_maximized_configure);
    assert(g_client.cfg_maximized);
    assert(g_client.cfg_w == kBoundsW && g_client.cfg_h == kBoundsH);
    return 0;
}
