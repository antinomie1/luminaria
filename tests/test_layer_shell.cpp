// zwlr_layer_shell_v1: a panel anchored across the top of an 800x600 output
// with a 32px exclusive zone. The compositor's arrange pass has to hand the
// client a full-width strip AND shrink the usable area for everyone else —
// getting the second half wrong is how maximized windows end up under the bar.
#include <cassert>
#include <cstddef>
#include <cstring>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

import luminaria;
import std;

namespace {

constexpr int kOutputW = 800;
constexpr int kOutputH = 600;
constexpr int kPanelH = 32;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    zwlr_layer_shell_v1* shell = nullptr;
    wl_surface* surface = nullptr;
    zwlr_layer_surface_v1* layer_surface = nullptr;
    bool buffered = false;
    int cfg_w = -1, cfg_h = -1;
    int configures = 0;
    bool closed = false;
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

void layer_surface_configure(void* data, zwlr_layer_surface_v1* ls, uint32_t serial,
                             uint32_t width, uint32_t height) {
    auto* st = static_cast<ClientState*>(data);
    st->cfg_w = static_cast<int>(width);
    st->cfg_h = static_cast<int>(height);
    ++st->configures;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    if (!st->buffered) {
        wl_surface_attach(st->surface, make_buffer(st, st->cfg_w, st->cfg_h), 0, 0);
        wl_surface_commit(st->surface);
        st->buffered = true;
    }
}
void layer_surface_closed(void* data, zwlr_layer_surface_v1*) {
    static_cast<ClientState*>(data)->closed = true;
}
const zwlr_layer_surface_v1_listener kLayerSurfaceListener{layer_surface_configure,
                                                           layer_surface_closed};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_shm") == 0) {
        st->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, "zwlr_layer_shell_v1") == 0) {
        st->shell = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 5));
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

    if (st.compositor != nullptr && st.shm != nullptr && st.shell != nullptr) {
        st.surface = wl_compositor_create_surface(st.compositor);
        st.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            st.shell, st.surface, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "panel");
        zwlr_layer_surface_v1_add_listener(st.layer_surface, &kLayerSurfaceListener, &st);
        // Width 0 = "as wide as you can make me", which is only legal because
        // the surface is anchored to both side edges.
        zwlr_layer_surface_v1_set_size(st.layer_surface, 0, kPanelH);
        zwlr_layer_surface_v1_set_anchor(st.layer_surface,
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                             ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                             ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(st.layer_surface, kPanelH);

        wl_surface_commit(st.surface); // initial commit -> first configure
        wl_display_roundtrip(display); // ack + buffer + map
        wl_display_roundtrip(display); // the server sees the mapping commit
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
    assert(display->init_shm().has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto shell = luminaria::LayerShell::create(*display);
    assert(shell.has_value());

    const luminaria::Box full{0, 0, kOutputW, kOutputH};
    luminaria::Box usable = full;
    luminaria::Box placed{};
    std::string seen_scope;
    bool mapped = false;
    bool is_top_layer = false;
    int arranges = 0;
    std::vector<luminaria::Signal<luminaria::LayerSurfaceStateChange>::Connection> state_conns;
    std::vector<luminaria::Signal<luminaria::LayerSurfaceMap>::Connection> map_conns;

    auto nls = shell->new_layer_surface().connect([&](luminaria::NewLayerSurface& e) {
        luminaria::LayerSurface* ls = &e.layer_surface;
        state_conns.push_back(
            ls->state_change.connect([&, ls](luminaria::LayerSurfaceStateChange&) {
                ++arranges;
                usable = full; // one surface, so the layout pass starts over
                placed = luminaria::arrange_layer_surface(*ls, full, usable);
                seen_scope = ls->scope();
                is_top_layer = ls->layer() == luminaria::Layer::top;
            }));
        map_conns.push_back(
            ls->map.connect([&](luminaria::LayerSurfaceMap&) { mapped = true; }));
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

    assert(arranges >= 1);
    assert(seen_scope == "panel");
    assert(is_top_layer);
    assert(mapped);
    // Full width, its own height, at the top.
    assert((placed == luminaria::Box{0, 0, kOutputW, kPanelH}));
    // …and the rest of the output is what everyone else may use.
    assert((usable == luminaria::Box{0, kPanelH, kOutputW, kOutputH - kPanelH}));

    // The client was told the size the compositor chose, not the 0 it asked for.
    assert(g_client.configures >= 1);
    assert(g_client.cfg_w == kOutputW && g_client.cfg_h == kPanelH);
    assert(!g_client.closed);
    return 0;
}
