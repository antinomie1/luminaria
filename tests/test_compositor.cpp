// Phase 2 slice 1: a real Wayland client (in-process, over a socketpair) binds
// wl_compositor, creates a wl_surface, and commits it. The server side asserts
// it saw new_surface + commit. No GPU, no parent compositor.
#include <cassert>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/socket.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"

namespace {

// ---- client thread: drives the fd handed to it, then exits ----
struct ClientState {
    wl_compositor* compositor = nullptr;
};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t /*version*/) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState st;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display); // receive globals
    if (st.compositor != nullptr) {
        wl_surface* surface = wl_compositor_create_surface(st.compositor);
        wl_surface_attach(surface, nullptr, 0, 0);
        wl_surface_commit(surface);
        wl_display_roundtrip(display); // ensure the server processes the commit
    }
    wl_display_disconnect(display); // server terminates on our disconnect
}

// Server-side: terminate on client disconnect (never mid-session).
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
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(rc == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());

    bool saw_surface = false;
    bool saw_commit = false;
    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> commit_conns;

    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        saw_surface = true;
        commit_conns.push_back(
            e.surface.commit.connect([&](luminaria::SurfaceCommit&) { saw_commit = true; }));
    });

    // Server takes fds[0]; client thread takes fds[1].
    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_client, fds[1]);

    // Safety net so the test can never hang the suite.
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(3000);

    display->run();
    client_thread.join();

    assert(saw_surface);
    assert(saw_commit);
    return 0;
}
