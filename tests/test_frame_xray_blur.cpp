// The protocol's committed blur region must turn into one texture placement
// immediately below its surface, with UVs addressed against the output cache.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "ext-background-effect-v1-client-protocol.h"

import luminaria.gpu;

namespace {
constexpr std::uint32_t kXrgb8888 = 0x34325258u; // DRM_FORMAT_XRGB8888

struct ClientState {
    wl_compositor* compositor = nullptr;
    ext_background_effect_manager_v1* effects = nullptr;
    wl_shm* shm = nullptr;
};

wl_buffer* make_buffer(ClientState& state) {
    constexpr int width = 16;
    constexpr int height = 16;
    constexpr int stride = width * 4;
    constexpr int size = stride * height;
    int fd = memfd_create("luminaria-test", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, size) == 0);
    auto* pixels = static_cast<std::uint32_t*>(
        mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(pixels != MAP_FAILED);
    for (int i = 0; i < width * height; ++i) {
        pixels[i] = 0x80FFFFFFu;
    }
    munmap(pixels, size);
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, size);
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                   WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

void registry_global(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                     std::uint32_t) {
    auto* state = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        state->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "ext_background_effect_manager_v1") == 0) {
        state->effects = static_cast<ext_background_effect_manager_v1*>(
            wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, 1));
    } else if (std::strcmp(interface, "wl_shm") == 0) {
        state->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
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
    if (state.compositor == nullptr || state.effects == nullptr || state.shm == nullptr) {
        wl_display_disconnect(display);
        return;
    }
    wl_surface* surface = wl_compositor_create_surface(state.compositor);
    auto* effect = ext_background_effect_manager_v1_get_background_effect(state.effects, surface);
    wl_region* region = wl_compositor_create_region(state.compositor);
    wl_region_add(region, 3, 4, 5, 6);
    ext_background_effect_surface_v1_set_blur_region(effect, region);
    wl_surface_attach(surface, make_buffer(state), 0, 0);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    ext_background_effect_surface_v1_destroy(effect);
    wl_region_destroy(region);
    wl_surface_destroy(surface);
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
    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77;
    }
    std::vector<std::uint8_t> pixels(64 * 48 * 4, 255);
    auto cache = renderer->upload_texture(64, 48, pixels);
    assert(cache.has_value());
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    auto display = luminaria::Display::create();
    assert(display.has_value());
    assert(display->init_shm().has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto effects = luminaria::BackgroundEffectManager::create(*display);
    assert(effects.has_value());
    luminaria::HeadlessOutput output(display->event_loop(), 64, 48, 1);
    luminaria::Frame frame(output, *renderer);
    assert(frame.reset(kXrgb8888).has_value());

    bool checked = false;
    luminaria::Signal<luminaria::SurfaceCommit>::Connection commits;
    auto surfaces = compositor->new_surface().connect([&](luminaria::NewSurface& event) {
        commits = event.surface.commit.connect([&](luminaria::SurfaceCommit& commit) {
            frame.begin({0, 0, 64, 48});
            frame.place_xray_blur(*cache, {0, 0, 64, 48}, commit.surface, 10, 11);
            frame.place(commit.surface, 10, 11);
            const std::span<const luminaria::Placement> placements = frame.placements();
            assert(placements.size() == 2);
            const luminaria::Placement& blur = placements[0];
            assert(blur.texture == &*cache && !blur.surface.valid());
            assert(blur.x == 13 && blur.y == 15 && blur.width == 5 && blur.height == 6);
            assert(blur.u0 == 13.0f / 64.0f && blur.v0 == 15.0f / 48.0f);
            assert(blur.u1 == 18.0f / 64.0f && blur.v1 == 21.0f / 48.0f);
            checked = true;
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
    assert(checked);
    return 0;
}
