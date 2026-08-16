// A deterministic malformed-protocol client that runs in the ordinary suite.
//
// This is deliberately not a libFuzzer target hidden behind a special build:
// every `xmake test` drives the same seeded request streams through a real
// libwayland client/server socketpair. Valid prefixes churn surface state,
// regions and wl_buffer lifetimes; each session then either injects a protocol
// error or exercises the short-stride layout that libwayland accepts but every
// Luminaria pixel reader must reject.
#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "xdg-shell-client-protocol.h"

import luminaria;
import std;

namespace {

void discard_expected_protocol_log(const char*, std::va_list) {}

enum class Mutation {
    valid_churn,
    short_stride,
    bad_scale,
    bad_transform,
    duplicate_xdg_role,
    self_subsurface,
};

class Rng {
public:
    explicit Rng(std::uint32_t seed) : state_(seed) {}

    std::uint32_t next() noexcept {
        state_ = state_ * 1664525u + 1013904223u;
        return state_;
    }

    int bounded(int limit) noexcept { return static_cast<int>(next() % static_cast<unsigned>(limit)); }

private:
    std::uint32_t state_;
};

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
};

struct ClientOutcome {
    bool globals_bound = false;
    bool prefix_ok = false;
    bool mutation_rejected = false;
};

struct ServerOutcome {
    int commits = 0;
    int rejected_layouts = 0;
    bool client_destroyed = false;
};

void registry_global(void* data, wl_registry* registry, std::uint32_t name,
                     const char* interface, std::uint32_t version) {
    auto* state = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        state->compositor = static_cast<wl_compositor*>(wl_registry_bind(
            registry, name, &wl_compositor_interface, std::min(version, 6u)));
    } else if (std::strcmp(interface, "wl_shm") == 0) {
        state->shm =
            static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, "wl_subcompositor") == 0) {
        state->subcompositor = static_cast<wl_subcompositor*>(
            wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (std::strcmp(interface, "xdg_wm_base") == 0) {
        state->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    }
}

void registry_global_remove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

wl_shm_pool* make_pool(wl_shm* shm, int bytes) {
    const int fd = memfd_create("luminaria-protocol-fuzz", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, bytes) == 0);
    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, bytes);
    close(fd);
    return pool;
}

wl_buffer* make_good_buffer(wl_shm_pool* pool) {
    constexpr int kWidth = 16;
    constexpr int kHeight = 16;
    constexpr int kStride = kWidth * 4;
    return wl_shm_pool_create_buffer(pool, 0, kWidth, kHeight, kStride,
                                     WL_SHM_FORMAT_ARGB8888);
}

void fuzz_valid_prefix(ClientState& state, wl_surface* surface, wl_shm_pool* pool, Rng& rng) {
    for (int step = 0; step < 48; ++step) {
        switch (rng.bounded(9)) {
        case 0:
            wl_surface_set_buffer_scale(surface, 1 + rng.bounded(4));
            break;
        case 1:
            wl_surface_set_buffer_transform(surface, rng.bounded(8));
            break;
        case 2:
            wl_surface_offset(surface, rng.bounded(33) - 16, rng.bounded(33) - 16);
            break;
        case 3:
            wl_surface_damage(surface, rng.bounded(49) - 16, rng.bounded(49) - 16,
                              1 + rng.bounded(32), 1 + rng.bounded(32));
            break;
        case 4:
            wl_surface_damage_buffer(surface, rng.bounded(49) - 16, rng.bounded(49) - 16,
                                     1 + rng.bounded(32), 1 + rng.bounded(32));
            break;
        case 5: {
            wl_region* region = wl_compositor_create_region(state.compositor);
            wl_region_add(region, rng.bounded(17) - 8, rng.bounded(17) - 8,
                          1 + rng.bounded(32), 1 + rng.bounded(32));
            wl_region_subtract(region, rng.bounded(17) - 8, rng.bounded(17) - 8,
                               1 + rng.bounded(16), 1 + rng.bounded(16));
            if ((rng.next() & 1u) == 0) {
                wl_surface_set_input_region(surface, region);
            } else {
                wl_surface_set_opaque_region(surface, region);
            }
            wl_region_destroy(region);
            break;
        }
        case 6:
            wl_surface_attach(surface, make_good_buffer(pool), 0, 0);
            break;
        case 7:
            wl_surface_commit(surface);
            break;
        case 8: {
            // Destroying a committed wl_buffer is legal and is the sharpest
            // resource-lifetime edge in the compositor.
            wl_buffer* buffer = make_good_buffer(pool);
            wl_surface_attach(surface, buffer, 0, 0);
            wl_surface_commit(surface);
            wl_buffer_destroy(buffer);
            break;
        }
        }
    }
    wl_surface_commit(surface);
}

void inject_mutation(ClientState& state, wl_surface* surface, wl_shm_pool* pool,
                     Mutation mutation) {
    switch (mutation) {
    case Mutation::valid_churn: {
        for (int i = 0; i < 12; ++i) {
            wl_buffer* buffer = make_good_buffer(pool);
            wl_surface_attach(surface, buffer, 0, 0);
            wl_surface_commit(surface);
            wl_buffer_destroy(buffer);
        }
        break;
    }
    case Mutation::short_stride: {
        // libwayland compares stride bytes against width pixels, so this 4x
        // undersized ARGB layout reaches Surface::current_buffer_rgba().
        constexpr int kWidth = 128;
        constexpr int kHeight = 64;
        constexpr int kStride = kWidth;
        wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, kWidth, kHeight, kStride,
                                                      WL_SHM_FORMAT_ARGB8888);
        wl_surface_set_buffer_scale(surface, 1);
        wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_NORMAL);
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_commit(surface);
        break;
    }
    case Mutation::bad_scale:
        wl_surface_set_buffer_scale(surface, 0);
        break;
    case Mutation::bad_transform:
        wl_surface_set_buffer_transform(surface, 99);
        break;
    case Mutation::duplicate_xdg_role: {
        xdg_surface* xsurface = xdg_wm_base_get_xdg_surface(state.wm_base, surface);
        (void)xdg_surface_get_toplevel(xsurface);
        (void)xdg_surface_get_toplevel(xsurface);
        break;
    }
    case Mutation::self_subsurface:
        (void)wl_subcompositor_get_subsurface(state.subcompositor, surface, surface);
        break;
    }
}

bool expects_protocol_error(Mutation mutation) {
    return mutation == Mutation::bad_scale || mutation == Mutation::bad_transform ||
           mutation == Mutation::duplicate_xdg_role || mutation == Mutation::self_subsurface;
}

void run_client(int fd, Mutation mutation, std::uint32_t seed, ClientOutcome& outcome) {
    wl_display* display = wl_display_connect_to_fd(fd);
    assert(display != nullptr);

    ClientState state;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistry, &state);
    assert(wl_display_roundtrip(display) >= 0);
    outcome.globals_bound = state.compositor != nullptr && state.shm != nullptr &&
                            state.subcompositor != nullptr && state.wm_base != nullptr;
    assert(outcome.globals_bound);

    // Large enough for the valid 16x16 buffers and the short-stride mutation.
    wl_shm_pool* pool = make_pool(state.shm, 64 * 1024);
    wl_surface* surface = wl_compositor_create_surface(state.compositor);
    Rng rng{seed};
    fuzz_valid_prefix(state, surface, pool, rng);
    outcome.prefix_ok = wl_display_roundtrip(display) >= 0;
    if (outcome.prefix_ok) {
        inject_mutation(state, surface, pool, mutation);
        outcome.mutation_rejected = wl_display_roundtrip(display) < 0;
    }
    wl_display_disconnect(display);
}

struct DestroyCtx {
    wl_listener listener;
    luminaria::Display* display;
    ServerOutcome* outcome;
};

void on_client_destroy(wl_listener* listener, void*) {
    auto* ctx = reinterpret_cast<DestroyCtx*>(reinterpret_cast<char*>(listener) -
                                              offsetof(DestroyCtx, listener));
    ctx->outcome->client_destroyed = true;
    ctx->display->terminate();
}

ServerOutcome run_case(Mutation mutation, std::uint32_t seed, ClientOutcome& client_outcome) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    assert(display->init_shm().has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto subcompositor = luminaria::Subcompositor::create(*display);
    assert(subcompositor.has_value());
    auto shell = luminaria::XdgShell::create(*display);
    assert(shell.has_value());

    ServerOutcome outcome;
    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> commits;
    auto new_surface = compositor->new_surface().connect([&](luminaria::NewSurface& event) {
        commits.push_back(event.surface.commit.connect([&](luminaria::SurfaceCommit& commit) {
            ++outcome.commits;
            luminaria::Surface& surface = commit.surface;
            assert(luminaria::surface_from_id(surface.id()) == &surface);

            std::vector<std::uint8_t> pixels;
            int width = 0;
            int height = 0;
            if (surface.has_buffer() &&
                !surface.current_buffer_rgba(pixels, width, height)) {
                ++outcome.rejected_layouts;
            }
            std::vector<luminaria::SurfaceAt> tree;
            surface.surface_tree(tree);
            (void)surface.accepts_input(0.5, 0.5);
        }));
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display, &outcome};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_client, fds[1], mutation, seed, std::ref(client_outcome));
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(2000);
    display->run();
    client_thread.join();
    return outcome;
}

} // namespace

int main() {
    // Four mutation classes intentionally make libwayland disconnect the
    // client. Their diagnostics are expected test data, not useful suite noise.
    wl_log_set_handler_client(discard_expected_protocol_log);
    wl_log_set_handler_server(discard_expected_protocol_log);

    constexpr Mutation kMutations[] = {
        Mutation::valid_churn,       Mutation::short_stride, Mutation::bad_scale,
        Mutation::bad_transform,    Mutation::duplicate_xdg_role,
        Mutation::self_subsurface,
    };

    for (Mutation mutation : kMutations) {
        for (std::uint32_t seed = 1; seed <= 8; ++seed) {
            ClientOutcome client;
            const ServerOutcome server =
                run_case(mutation, 0x9e3779b9u * seed, client);
            assert(client.globals_bound);
            assert(client.prefix_ok);
            assert(server.client_destroyed);
            assert(server.commits > 0);
            assert(client.mutation_rejected == expects_protocol_error(mutation));
            if (mutation == Mutation::short_stride) {
                assert(server.rejected_layouts > 0);
            }
        }
    }
    return 0;
}
