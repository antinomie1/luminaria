// ext-background-effect-v1 is only a double-buffered blur hint. The compositor
// decides whether it renders it, but the region must arrive on Surface exactly
// with the carrying wl_surface.commit and disappear on NULL/object destruction.
#include <cassert>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "ext-background-effect-v1-client-protocol.h"

import luminaria;

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    ext_background_effect_manager_v1* manager = nullptr;
};

void registry_global(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                     std::uint32_t) {
    auto* state = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        state->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "ext_background_effect_manager_v1") == 0) {
        state->manager = static_cast<ext_background_effect_manager_v1*>(
            wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, std::uint32_t) {}
constexpr wl_registry_listener registry_listener{registry_global, registry_global_remove};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState state;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    wl_display_roundtrip(display);
    if (state.compositor == nullptr || state.manager == nullptr) {
        wl_display_disconnect(display);
        return;
    }
    wl_surface* surface = wl_compositor_create_surface(state.compositor);
    ext_background_effect_surface_v1* effect =
        ext_background_effect_manager_v1_get_background_effect(state.manager, surface);
    wl_region* region = wl_compositor_create_region(state.compositor);
    wl_region_add(region, 3, 4, 5, 6);

    ext_background_effect_surface_v1_set_blur_region(effect, region);
    wl_display_roundtrip(display); // still pending: no wl_surface.commit yet
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    ext_background_effect_surface_v1_set_blur_region(effect, nullptr);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    ext_background_effect_surface_v1_set_blur_region(effect, region);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    ext_background_effect_surface_v1_destroy(effect);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    wl_region_destroy(region);
    wl_surface_destroy(surface);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
}

struct DestroyCtx {
    wl_listener listener;
    luminaria::Display* display = nullptr;
};
void client_destroyed(wl_listener* listener, void*) {
    auto* context = reinterpret_cast<DestroyCtx*>(reinterpret_cast<char*>(listener) -
                                                  offsetof(DestroyCtx, listener));
    context->display->terminate();
}

} // namespace

int main() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto effects = luminaria::BackgroundEffectManager::create(*display);
    assert(effects.has_value());

    std::vector<bool> seen;
    luminaria::Signal<luminaria::SurfaceCommit>::Connection commits;
    auto surfaces = compositor->new_surface().connect([&](luminaria::NewSurface& event) {
        assert(event.surface.blur_region().empty());
        commits = event.surface.commit.connect([&](luminaria::SurfaceCommit& commit) {
            const luminaria::Region& blur = commit.surface.blur_region();
            seen.push_back(!blur.empty());
            if (!blur.empty()) {
                assert(blur.rects().size() == 1);
                assert((blur.rects().front() == luminaria::Box{3, 4, 5, 6}));
            }
        });
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx context{{}, &*display};
    context.listener.notify = client_destroyed;
    wl_client_add_destroy_listener(client, &context.listener);

    std::thread thread(run_client, fds[1]);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(3000);
    display->run();
    thread.join();

    assert((seen == std::vector<bool>{true, false, true, false}));
    return 0;
}
