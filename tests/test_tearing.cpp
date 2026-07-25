// wp_tearing_control_v1: the client's presentation hint must reach the Surface,
// and only on commit — it is double-buffered state like everything else.
#include <cassert>
#include <cstddef>
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
#include "luminaria/tearing_control.hpp"
#include "tearing-control-v1-client-protocol.h"

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wp_tearing_control_manager_v1* manager = nullptr;
};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "wl_compositor") == 0) {
        st->compositor =
            static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(iface, "wp_tearing_control_manager_v1") == 0) {
        st->manager = static_cast<wp_tearing_control_manager_v1*>(
            wl_registry_bind(reg, name, &wp_tearing_control_manager_v1_interface, 1));
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
    assert(st.compositor != nullptr && st.manager != nullptr);

    wl_surface* surface = wl_compositor_create_surface(st.compositor);
    wl_surface_commit(surface); // commit #1: still vsync
    wl_display_roundtrip(display);

    wp_tearing_control_v1* control =
        wp_tearing_control_manager_v1_get_tearing_control(st.manager, surface);
    wp_tearing_control_v1_set_presentation_hint(control,
                                                WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
    wl_display_roundtrip(display); // hint sent but not committed yet
    wl_surface_commit(surface);    // commit #2: now it applies
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

std::vector<bool> g_hints; // tearing_hint() after each commit

} // namespace

int main() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto tearing = luminaria::TearingControlManager::create(*display);
    assert(tearing.has_value());

    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* s = &e.surface;
        conns.push_back(e.surface.commit.connect(
            [s](luminaria::SurfaceCommit&) { g_hints.push_back(s->tearing_hint()); }));
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

    assert(g_hints.size() == 2);
    assert(!g_hints[0]); // default is vsync
    assert(g_hints[1]);  // async, applied by the commit that followed the hint
    return 0;
}
