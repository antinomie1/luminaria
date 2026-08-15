// wp_commit_timing_v1. A commit stamped for the future must not be applied
// early, and — the part that needs a real event loop — it must be applied
// WITHOUT the client saying anything else. Nobody else is going to wake up for
// it, so the global's own timer has to.
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "commit-timing-v1-client-protocol.h"

import luminaria;

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wp_commit_timing_manager_v1* timing = nullptr;
};

ClientState g_client;

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wp_commit_timing_manager_v1") == 0) {
        st->timing = static_cast<wp_commit_timing_manager_v1*>(
            wl_registry_bind(registry, name, &wp_commit_timing_manager_v1_interface, 1));
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
    if (st.compositor == nullptr || st.timing == nullptr) {
        wl_display_disconnect(display);
        return;
    }

    wl_surface* surface = wl_compositor_create_surface(st.compositor);
    wp_commit_timer_v1* timer = wp_commit_timing_manager_v1_get_timer(st.timing, surface);
    wl_display_roundtrip(display);

    // Due 250ms from now, on the same CLOCK_MONOTONIC the compositor reads.
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    auto ns = static_cast<std::uint64_t>(ts.tv_nsec) + 250'000'000ULL;
    auto sec = static_cast<std::uint64_t>(ts.tv_sec) + ns / 1'000'000'000ULL;
    wp_commit_timer_v1_set_timestamp(timer, static_cast<uint32_t>(sec >> 32),
                                     static_cast<uint32_t>(sec & 0xFFFFFFFFU),
                                     static_cast<uint32_t>(ns % 1'000'000'000ULL));
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    // Say nothing more: the release must come from the compositor's own timer.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    wp_commit_timer_v1_destroy(timer);
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
    auto timing = luminaria::CommitTimingManager::create(*display);
    assert(timing.has_value());

    int commits = 0;
    luminaria::Surface* the_surface = nullptr;
    luminaria::Signal<luminaria::SurfaceCommit>::Connection on_commit;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        the_surface = &e.surface;
        on_commit = e.surface.commit.connect([&](luminaria::SurfaceCommit&) { ++commits; });
    });

    int commits_early = -1;
    bool parked = false;
    auto early = display->event_loop().add_timer([&] {
        commits_early = commits;
        parked = the_surface != nullptr && the_surface->has_deferred_commit();
    });
    early.arm(120);

    int commits_late = -1;
    auto late = display->event_loop().add_timer([&] { commits_late = commits; });
    late.arm(450);

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

    assert(timing->timer_count() == 0); // the client destroyed its timer
    assert(commits_early == 0);         // not applied before its stamp
    assert(parked);
    assert(commits_late == 1);          // applied by the manager's own wakeup
    return 0;
}
