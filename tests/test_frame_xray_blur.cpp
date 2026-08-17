// The protocol's committed blur region must turn into one texture placement
// immediately below its surface, with UVs addressed against the backdrop —
// and, once it is a real blur, the pixels inside a declared region must mix
// while the pixels outside it stay exactly where they were. A client that
// declares nothing must not buy a backdrop capture at all.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <sys/socket.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "ext-background-effect-v1-client-protocol.h"

import luminaria.gpu;
import std;

namespace {
constexpr std::uint32_t kXrgb8888 = 0x34325258u; // DRM_FORMAT_XRGB8888
constexpr int kW = 64, kH = 48;
constexpr int kEdge = 32; // the two backdrop halves meet here

const luminaria::Pixel& at(const std::vector<luminaria::Pixel>& px, int x, int y) {
    return px[static_cast<std::size_t>(y) * kW + x];
}

struct ClientState {
    wl_compositor* compositor = nullptr;
    ext_background_effect_manager_v1* effects = nullptr;
    wl_shm* shm = nullptr;
};

// Fully transparent, so the blur regions show the backdrop through it: the
// assertions are about the region walk, not about the surface's own pixels.
wl_buffer* make_buffer(ClientState& state) {
    constexpr int width = kW;
    constexpr int height = kH;
    constexpr int stride = width * 4;
    constexpr int size = stride * height;
    int fd = memfd_create("luminaria-test", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, size) == 0);
    auto* pixels = static_cast<std::uint32_t*>(
        mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(pixels != MAP_FAILED);
    std::memset(pixels, 0, size); // ARGB 0x00000000: nothing visible
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

// Three commits: (3,4,5,6) for the placement geometry, then a region straddling
// the red/blue edge for the pixel assertions, then an empty region. Each one
// reaches the server as a separate SurfaceCommit, which the main thread counts.
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
    wl_surface_attach(surface, make_buffer(state), 0, 0);

    wl_region* region = wl_compositor_create_region(state.compositor);
    wl_region_add(region, 3, 4, 5, 6);
    ext_background_effect_surface_v1_set_blur_region(effect, region);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    wl_region_destroy(region);
    region = wl_compositor_create_region(state.compositor);
    wl_region_add(region, 20, 10, 24, 20); // straddles the edge at x=32
    ext_background_effect_surface_v1_set_blur_region(effect, region);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    wl_region_destroy(region);
    region = wl_compositor_create_region(state.compositor);
    ext_background_effect_surface_v1_set_blur_region(effect, region); // empty
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    wl_region_destroy(region);
    ext_background_effect_surface_v1_destroy(effect);
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
    std::vector<std::uint8_t> pixels(kW * kH * 4, 255);
    auto cache = renderer->upload_texture(kW, kH, pixels);
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
    luminaria::HeadlessOutput output(display->event_loop(), kW, kH, 1);
    luminaria::Frame frame(output, *renderer);
    assert(frame.reset(kXrgb8888).has_value());

    auto backdrop = renderer->create_offscreen(kW, kH);
    auto chain = renderer->create_blur_chain(kW, kH, 4);
    if (!backdrop || !chain) {
        std::fprintf(stderr, "skip: no offscreen/blur targets\n");
        return 77;
    }

    const luminaria::BlurParams params{.passes = 2, .offset = 2.0f};
    const int spread = luminaria::blur_spread(params);
    const luminaria::Box view{0, 0, kW, kH};
    const luminaria::Box item_box{0, 0, kW, kH};
    const luminaria::Color black{0, 0, 0, 1};

    int phase = 0;
    bool checked = false;
    luminaria::Signal<luminaria::SurfaceCommit>::Connection commits;
    auto surfaces = compositor->new_surface().connect([&](luminaria::NewSurface& event) {
        commits = event.surface.commit.connect([&](luminaria::SurfaceCommit& commit) {
            frame.begin(view);
            if (phase == 0) {
                // Placement geometry only: one region translated by the root's
                // origin, clipped, and addressed against the backdrop.
                const bool placed = frame.place_blur_regions(*cache, view, commit.surface, 10,
                                                             11, item_box, 0);
                assert(placed);
                frame.place(commit.surface, 10, 11);
                const std::span<const luminaria::Placement> placements = frame.placements();
                assert(placements.size() == 2);
                const luminaria::Placement& blur = placements[0];
                assert(blur.texture == &*cache && !blur.surface.valid());
                assert(blur.x == 13 && blur.y == 15 && blur.width == 5 && blur.height == 6);
                assert(blur.u0 == 13.0f / kW && blur.v0 == 15.0f / kH);
                assert(blur.u1 == 18.0f / kW && blur.v1 == 21.0f / kH);
                ++phase;
            } else if (phase == 1) {
                // The real thing: the region straddles the red/blue edge, so
                // pixels inside it mix while pixels outside it stay untouched.
                const luminaria::PlacementGroup below = frame.begin_group();
                frame.place_rect(0, 0, kEdge, kH, luminaria::Color{1, 0, 0, 1});
                frame.place_rect(kEdge, 0, kW - kEdge, kH, luminaria::Color{0, 0, 1, 1});
                assert(frame.capture_blur(below, *backdrop, view, *chain, params).has_value());
                assert(frame.place_blur_regions(chain->texture(), view, commit.surface, 10, 11,
                                                item_box, spread));
                frame.place(commit.surface, 10, 11);
                assert(frame.submit(black) == luminaria::Presented::composited);

                const std::vector<luminaria::Pixel>& px = output.last_frame();
                // Outside the surface entirely: the red half is untouched.
                assert(at(px, 4, 32).r > 200 && at(px, 4, 32).b < 40);
                // Inside the declared region, on the blue side of the edge,
                // red has bled across — only the region is blurred.
                const luminaria::Pixel mixed = at(px, 36, 30);
                assert(mixed.r > 30 && mixed.b > 30);
                // Under the surface but outside the region, the same blue side
                // still shows the sharp edge: no blur reached here.
                const luminaria::Pixel plain = at(px, 58, 30);
                assert(plain.b > 200 && plain.r < 40);
                ++phase;
            } else if (phase == 2) {
                // An empty client request places nothing and captures nothing:
                // the walk answers false before any capture is paid for.
                const bool placed = frame.place_blur_regions(*cache, view, commit.surface, 10,
                                                             11, item_box, 0);
                assert(!placed);
                assert(frame.placements().empty());
                frame.place(commit.surface, 10, 11);
                assert(frame.submit(black) == luminaria::Presented::composited);
                // The blur-free frame is not what keeps a desktop awake.
                assert(frame.submit(black) == luminaria::Presented::unchanged);
                ++phase;
                checked = true;
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
    assert(checked);
    assert(phase == 3);
    return 0;
}
