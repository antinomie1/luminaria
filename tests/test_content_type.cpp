// wp_content_type_v1. One hint, double-buffered: it must not take effect until
// the commit that carries it, and destroying the object must put it back to
// none — a video player that closed its content-type object should not leave
// the compositor treating it as a video forever.
#include <cassert>
#include <cstdint>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "content-type-v1-client-protocol.h"

import luminaria;
import std;

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wp_content_type_manager_v1* manager = nullptr;
};

ClientState g_client;

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wp_content_type_manager_v1") == 0) {
        st->manager = static_cast<wp_content_type_manager_v1*>(
            wl_registry_bind(registry, name, &wp_content_type_manager_v1_interface, 1));
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
    wp_content_type_v1* ct =
        wp_content_type_manager_v1_get_surface_content_type(st.manager, surface);

    // Staged, not committed: the server must still read `none` here.
    wp_content_type_v1_set_content_type(ct, WP_CONTENT_TYPE_V1_TYPE_VIDEO);
    wl_display_roundtrip(display);

    wl_surface_commit(surface); // now it applies
    wl_display_roundtrip(display);

    wp_content_type_v1_destroy(ct);
    wl_surface_commit(surface); // ...and destruction resets it
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
    auto content = luminaria::ContentTypeManager::create(*display);
    assert(content.has_value());

    std::vector<std::uint32_t> seen;
    bool none_before_commit = false;
    luminaria::Signal<luminaria::SurfaceCommit>::Connection on_commit;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        none_before_commit = e.surface.content_type() ==
                             static_cast<std::uint32_t>(luminaria::ContentType::none);
        on_commit = e.surface.commit.connect(
            [&](luminaria::SurfaceCommit& c) { seen.push_back(c.surface.content_type()); });
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

    assert(none_before_commit);
    assert((seen == std::vector<std::uint32_t>{
                       static_cast<std::uint32_t>(luminaria::ContentType::video),
                       static_cast<std::uint32_t>(luminaria::ContentType::none)}));
    return 0;
}
