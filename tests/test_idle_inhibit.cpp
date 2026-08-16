// zwp_idle_inhibit_manager_v1. The protocol sends nothing back, so everything
// interesting is server-side: an inhibitor appears and the screen must stay
// awake, marking its surface off-screen releases that without destroying it,
// and destroying it releases it for good. `changed` must fire only on the
// transitions — a blanking timer re-armed by every no-op would never blank.
#include <cassert>
#include <cstddef>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "idle-inhibit-unstable-v1-client-protocol.h"

import luminaria;
import std;

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    zwp_idle_inhibit_manager_v1* manager = nullptr;
};

ClientState g_client;

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "zwp_idle_inhibit_manager_v1") == 0) {
        st->manager = static_cast<zwp_idle_inhibit_manager_v1*>(
            wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, 1));
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

    if (st.compositor == nullptr || st.manager == nullptr) {
        wl_display_disconnect(display);
        return;
    }

    wl_surface* surface = wl_compositor_create_surface(st.compositor);
    zwp_idle_inhibitor_v1* inhibitor =
        zwp_idle_inhibit_manager_v1_create_inhibitor(st.manager, surface);
    wl_display_roundtrip(display);

    // A second commit is the server's cue to run the "hidden, then shown again"
    // half of the test.
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    zwp_idle_inhibitor_v1_destroy(inhibitor);
    wl_display_roundtrip(display);

    wl_surface_destroy(surface);
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

} // namespace

int main() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto idle = luminaria::IdleInhibitManager::create(*display);
    assert(idle.has_value());

    assert(!idle->inhibited());
    assert(idle->inhibitor_count() == 0);

    int new_inhibitors = 0;
    bool surface_matches = false;
    bool inhibited_after_create = false;
    std::size_t count_after_create = 0;
    luminaria::IdleInhibitor* seen = nullptr;
    luminaria::Surface* the_surface = nullptr;

    std::vector<bool> transitions;
    auto ch = idle->changed().connect(
        [&](luminaria::IdleInhibitChange& e) { transitions.push_back(e.inhibited); });

    auto ni = idle->new_inhibitor().connect([&](luminaria::NewIdleInhibitor& e) {
        ++new_inhibitors;
        seen = &e.inhibitor;
        surface_matches = &e.inhibitor.surface() == the_surface;
        // The signal fires before the manager recomputes, so this is still the
        // pre-inhibitor answer — the transition below is the observable one.
        inhibited_after_create = idle->inhibited();
        count_after_create = idle->inhibitor_count();
    });

    bool released_when_hidden = false;
    bool held_when_shown_again = false;
    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        the_surface = &e.surface;
        conns.push_back(e.surface.commit.connect([&](luminaria::SurfaceCommit&) {
            assert(seen != nullptr);
            seen->set_visible(false);
            released_when_hidden = !idle->inhibited();
            seen->set_visible(true);
            held_when_shown_again = idle->inhibited();
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

    assert(new_inhibitors == 1);
    assert(surface_matches);
    assert(!inhibited_after_create);
    assert(count_after_create == 1);
    assert(released_when_hidden);
    assert(held_when_shown_again);

    // on (created), off (hidden), on (shown), off (destroyed) — and nothing else.
    assert((transitions == std::vector<bool>{true, false, true, false}));
    assert(!idle->inhibited());
    assert(idle->inhibitor_count() == 0);
    return 0;
}
