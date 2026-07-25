// The three globals a HiDPI desktop needs on top of integer output scale:
// wp_viewporter (crop + stretch), wp_fractional_scale_v1 (the real scale, in
// 120ths), and zxdg_decoration_manager_v1 (who draws the title bar).
#include <cassert>
#include <cstddef>
#include <cstring>
#include <thread>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"
#include "luminaria/fractional_scale.hpp"
#include "luminaria/viewporter.hpp"
#include "luminaria/xdg_decoration.hpp"
#include "luminaria/xdg_shell.hpp"

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_fractional_scale_manager_v1* fractional = nullptr;
    zxdg_decoration_manager_v1* decoration = nullptr;
    xdg_wm_base* wm_base = nullptr;
    uint32_t preferred_scale = 0;
    uint32_t decoration_mode = 0;
    int decoration_configures = 0;
};

void wm_base_ping(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
const xdg_wm_base_listener kWmBase{wm_base_ping};

void on_preferred_scale(void* data, wp_fractional_scale_v1*, uint32_t scale) {
    static_cast<ClientState*>(data)->preferred_scale = scale;
}
const wp_fractional_scale_v1_listener kFractional{on_preferred_scale};

void on_decoration_configure(void* data, zxdg_toplevel_decoration_v1*, uint32_t mode) {
    auto* st = static_cast<ClientState*>(data);
    st->decoration_mode = mode;
    ++st->decoration_configures;
}
const zxdg_toplevel_decoration_v1_listener kDecoration{on_decoration_configure};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface,
                     uint32_t version) {
    auto* st = static_cast<ClientState*>(data);
    auto bind = [&](const wl_interface* interface, uint32_t want) {
        return wl_registry_bind(reg, name, interface, want < version ? want : version);
    };
    if (std::strcmp(iface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(bind(&wl_compositor_interface, 6));
    } else if (std::strcmp(iface, "wl_shm") == 0) {
        st->shm = static_cast<wl_shm*>(bind(&wl_shm_interface, 1));
    } else if (std::strcmp(iface, "wp_viewporter") == 0) {
        st->viewporter = static_cast<wp_viewporter*>(bind(&wp_viewporter_interface, 1));
    } else if (std::strcmp(iface, "wp_fractional_scale_manager_v1") == 0) {
        st->fractional = static_cast<wp_fractional_scale_manager_v1*>(
            bind(&wp_fractional_scale_manager_v1_interface, 1));
    } else if (std::strcmp(iface, "zxdg_decoration_manager_v1") == 0) {
        st->decoration = static_cast<zxdg_decoration_manager_v1*>(
            bind(&zxdg_decoration_manager_v1_interface, 1));
    } else if (std::strcmp(iface, "xdg_wm_base") == 0) {
        st->wm_base = static_cast<xdg_wm_base*>(bind(&xdg_wm_base_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

wl_buffer* make_buffer(wl_shm* shm, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    int fd = memfd_create("lum-test", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, size) == 0);
    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState st;
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistry, &st);
    wl_display_roundtrip(display);
    assert(st.compositor != nullptr && st.shm != nullptr && st.viewporter != nullptr);
    assert(st.fractional != nullptr && st.decoration != nullptr && st.wm_base != nullptr);
    xdg_wm_base_add_listener(st.wm_base, &kWmBase, nullptr);

    wl_surface* surface = wl_compositor_create_surface(st.compositor);
    wp_fractional_scale_v1* scale =
        wp_fractional_scale_manager_v1_get_fractional_scale(st.fractional, surface);
    wp_fractional_scale_v1_add_listener(scale, &kFractional, &st);

    // A decoration object gets an unsolicited configure straight away, so the
    // client knows whether to draw a frame before it renders anything.
    xdg_surface* xdg = xdg_wm_base_get_xdg_surface(st.wm_base, surface);
    xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdg);
    zxdg_toplevel_decoration_v1* deco =
        zxdg_decoration_manager_v1_get_toplevel_decoration(st.decoration, toplevel);
    zxdg_toplevel_decoration_v1_add_listener(deco, &kDecoration, &st);
    wl_display_roundtrip(display);
    assert(st.decoration_configures == 1);
    assert(st.decoration_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

    // Asking for client-side must be answered, and here the compositor agrees.
    zxdg_toplevel_decoration_v1_set_mode(deco,
                                         ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    wl_display_roundtrip(display);
    assert(st.decoration_configures == 2);
    assert(st.decoration_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);

    // A 100x50 buffer, cropped to its left half and stretched to 300x200.
    wp_viewport* viewport = wp_viewporter_get_viewport(st.viewporter, surface);
    wl_buffer* buffer = make_buffer(st.shm, 100, 50);
    wp_viewport_set_source(viewport, wl_fixed_from_int(0), wl_fixed_from_int(0),
                           wl_fixed_from_int(50), wl_fixed_from_int(50));
    wp_viewport_set_destination(viewport, 300, 200);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    assert(st.preferred_scale == 180); // 1.5x, set by the server below

    wl_buffer_destroy(buffer);
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
    assert(wl_display_init_shm(display->c_ptr()) == 0);
    auto compositor = luminaria::Compositor::create(*display);
    auto shell = luminaria::XdgShell::create(*display);
    auto viewporter = luminaria::Viewporter::create(*display);
    auto fractional = luminaria::FractionalScaleManager::create(*display);
    auto decoration = luminaria::XdgDecorationManager::create(*display);
    assert(compositor && shell && viewporter && fractional && decoration);

    // Default server-side, but honour a client that asks for client-side.
    decoration->set_default_mode(luminaria::DecorationMode::server_side);
    auto deco_conn = decoration->request().connect([](luminaria::DecorationRequest& r) {
        if (r.preferred.has_value()) {
            r.mode = *r.preferred;
        }
    });

    int commits = 0;
    luminaria::Signal<luminaria::SurfaceCommit>::Connection commit_conn;
    auto new_surface = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        commit_conn = e.surface.commit.connect([&](luminaria::SurfaceCommit& ce) {
            luminaria::Surface& s = ce.surface;
            ++commits;
            // 1.5x: no integer scale can say this, which is the whole point of
            // wp_fractional_scale_v1. Sent on commit, not on surface creation —
            // the client hasn't asked for a scale object yet at that point.
            fractional->set_scale(s, 180);
            // The buffer is 100x50; the viewport says the surface is 300x200.
            assert(s.buffer_width() == 100 && s.buffer_height() == 50);
            assert(s.surface_width() == 300 && s.surface_height() == 200);
            assert(s.has_viewport_source());
            // The crop is the left half of the buffer horizontally, all of it
            // vertically — in normalized buffer coordinates, ready for the
            // renderer.
            float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
            s.buffer_source_uv(u0, v0, u1, v1);
            assert(u0 == 0.0f && v0 == 0.0f);
            assert(u1 == 0.5f && v1 == 1.0f);
        });
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx dc{{}, &*display};
    dc.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &dc.listener);

    std::thread client_thread(run_client, fds[1]);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(3000);
    display->run();
    client_thread.join();
    assert(commits == 1);
    return 0;
}
