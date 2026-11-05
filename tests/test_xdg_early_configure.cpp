// A compositor that sizes a toplevel the moment it appears, before the client's
// initial commit, as tiling compositors do: the configure answering that
// commit, and every state change after it, must keep the size rather than send
// 0x0.
#include <cassert>
#include <cstddef>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-server-core.h>

#include "xdg-shell-client-protocol.h"

import luminaria;
import std;

namespace {

constexpr int kW = 622;
constexpr int kH = 776;

struct ClientState {
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    std::vector<std::pair<int, int>> sizes; // every xdg_toplevel.configure
};

void xdg_surface_configure(void*, xdg_surface* xsurf, uint32_t serial) {
    xdg_surface_ack_configure(xsurf, serial);
}
const xdg_surface_listener kXdgSurfaceListener{xdg_surface_configure};

void toplevel_configure(void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
    static_cast<ClientState*>(data)->sizes.emplace_back(w, h);
}
void toplevel_close(void*, xdg_toplevel*) {}
const xdg_toplevel_listener kToplevelListener{toplevel_configure, toplevel_close};

void wm_base_ping(void*, xdg_wm_base* wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}
const xdg_wm_base_listener kWmBaseListener{wm_base_ping};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* iface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(iface, "xdg_wm_base") == 0) {
        st->wm_base =
            static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 3));
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
    if (st.compositor != nullptr && st.wm_base != nullptr) {
        xdg_wm_base_add_listener(st.wm_base, &kWmBaseListener, &st);
        wl_surface* surface = wl_compositor_create_surface(st.compositor);
        xdg_surface* xsurf = xdg_wm_base_get_xdg_surface(st.wm_base, surface);
        xdg_surface_add_listener(xsurf, &kXdgSurfaceListener, &st);
        xdg_toplevel* toplevel = xdg_surface_get_toplevel(xsurf);
        xdg_toplevel_add_listener(toplevel, &kToplevelListener, &st);
        wl_display_roundtrip(display); // the compositor sizes it now
        wl_surface_commit(surface);    // initial commit
        wl_display_roundtrip(display);
        wl_display_roundtrip(display); // the activation it triggered
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
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto shell = luminaria::XdgShell::create(*display);
    assert(shell.has_value());

    luminaria::Toplevel* window = nullptr;
    auto nt = shell->new_toplevel().connect([&](luminaria::NewToplevel& e) {
        window = &e.toplevel;
        window->configure(luminaria::Size{kW, kH});
        window->set_tiled(true);
    });
    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> commits;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        commits.push_back(e.surface.commit.connect([&](luminaria::SurfaceCommit&) {
            if (window != nullptr) {
                window->set_activated(true);
            }
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

    assert(g_client.sizes.size() >= 3); // sized, tiled, initial commit, ...
    for (const auto& [w, h] : g_client.sizes) {
        assert(w == kW && h == kH);
    }
    return 0;
}
