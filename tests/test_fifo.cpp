// wp_fifo_v1. The whole protocol is one gate, so that is what this checks: a
// commit that waits on a barrier is INVISIBLE until the barrier clears — no
// commit signal, nothing for the compositor to draw — and presenting the
// surface (which is what send_frame_done means) lets it through.
//
// A client that got this wrong would either freeze after one frame or run free,
// which are the two failure modes the protocol exists to prevent.
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "fifo-v1-client-protocol.h"

import luminaria;

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wp_fifo_manager_v1* fifo = nullptr;
};

ClientState g_client;

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wp_fifo_manager_v1") == 0) {
        st->fifo = static_cast<wp_fifo_manager_v1*>(
            wl_registry_bind(registry, name, &wp_fifo_manager_v1_interface, 1));
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
    if (st.compositor == nullptr || st.fifo == nullptr) {
        wl_display_disconnect(display);
        return;
    }

    wl_surface* surface = wl_compositor_create_surface(st.compositor);
    wp_fifo_v1* fifo = wp_fifo_manager_v1_get_fifo(st.fifo, surface);
    wl_display_roundtrip(display);

    // Frame 1: takes the barrier, applies immediately (nothing to wait for).
    wp_fifo_v1_set_barrier(fifo);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    // Frame 2: waits for frame 1 to be shown. Must NOT apply yet.
    wp_fifo_v1_set_barrier(fifo);
    wp_fifo_v1_wait_barrier(fifo);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    // Hold the connection open long enough for the server's timers below.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    wp_fifo_v1_destroy(fifo);
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
    auto fifo = luminaria::FifoManager::create(*display);
    assert(fifo.has_value());

    int commits = 0;
    luminaria::Surface* the_surface = nullptr;
    luminaria::Signal<luminaria::SurfaceCommit>::Connection on_commit;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        the_surface = &e.surface;
        on_commit = e.surface.commit.connect([&](luminaria::SurfaceCommit&) { ++commits; });
    });

    // Both commits are in by 200ms; only the first may have been applied, and
    // the barrier it took must still be owed.
    int commits_before = -1;
    bool barrier_held = false;
    std::size_t fifos_alive = 0;
    auto probe = display->event_loop().add_timer([&] {
        commits_before = commits;
        barrier_held = the_surface != nullptr && the_surface->fifo_barrier();
        fifos_alive = fifo->fifo_count();
    });
    probe.arm(200);

    // Presenting the surface pays the debt, which must let frame 2 through.
    int commits_after = -1;
    bool deferred_before_release = false;
    auto release = display->event_loop().add_timer([&] {
        deferred_before_release = the_surface != nullptr && the_surface->has_deferred_commit();
        if (the_surface != nullptr) {
            the_surface->send_frame_done(0);
        }
        commits_after = commits;
    });
    release.arm(300);

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_client, fds[1]);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(5000);

    display->run();
    client_thread.join();

    assert(fifos_alive == 1);
    assert(commits_before == 1);       // frame 2 is parked, not applied
    assert(barrier_held);              // frame 1 still owes a presentation
    assert(deferred_before_release);
    assert(commits_after == 2);        // presenting frame 1 released frame 2
    assert(commits == 2);
    // Frame 2 took a barrier of its own, and nothing has presented it.
    return 0;
}
