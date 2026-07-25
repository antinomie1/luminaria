// A real client mints a wp_single_pixel_buffer_v1 buffer and attaches it; the
// server must see a 1x1 surface carrying exactly that colour.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"
#include "luminaria/single_pixel_buffer.hpp"
#include "single-pixel-buffer-v1-client-protocol.h"

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wp_single_pixel_buffer_manager_v1* manager = nullptr;
};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "wl_compositor") == 0) {
        st->compositor =
            static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(iface, "wp_single_pixel_buffer_manager_v1") == 0) {
        st->manager = static_cast<wp_single_pixel_buffer_manager_v1*>(
            wl_registry_bind(reg, name, &wp_single_pixel_buffer_manager_v1_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState st;
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistry, &st);
    wl_display_roundtrip(display);

    if (st.compositor != nullptr && st.manager != nullptr) {
        // Channels are 32-bit fractions of UINT32_MAX; the top byte is what an
        // 8-bit pipeline keeps.
        wl_buffer* buffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
            st.manager, 0x40000000u, 0x80000000u, 0xC0000000u, 0xFFFFFFFFu);
        wl_surface* surface = wl_compositor_create_surface(st.compositor);
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_commit(surface);
        wl_display_roundtrip(display);
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

bool g_seen = false;
std::vector<std::uint8_t> g_rgba;
int g_w = 0, g_h = 0;

} // namespace

int main() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto single_pixel = luminaria::SinglePixelBufferManager::create(*display);
    assert(single_pixel.has_value());

    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* s = &e.surface;
        conns.push_back(e.surface.commit.connect([&, s](luminaria::SurfaceCommit&) {
            if (s->current_buffer_rgba(g_rgba, g_w, g_h)) {
                g_seen = true;
                // The extent must be right without decoding pixels too.
                assert(s->buffer_width() == 1 && s->buffer_height() == 1);
            }
        }));
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

    assert(g_seen);
    assert(g_w == 1 && g_h == 1);
    assert(g_rgba.size() == 4);
    assert(g_rgba[0] == 0x40 && g_rgba[1] == 0x80 && g_rgba[2] == 0xC0 && g_rgba[3] == 0xFF);
    return 0;
}
