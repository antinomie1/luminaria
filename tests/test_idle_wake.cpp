// The other half of the low-power step, end to end: a screen that stops
// flipping must still come back.
//
// A client here does what every toolkit does — draw, ask for a frame callback,
// and wait for it before drawing again. If the compositor stops committing
// while nothing changes (which is the point) and nothing wakes it when the
// client commits, that client waits forever and the desktop is frozen. So this
// asserts both halves at once:
//
//   * the client completes every one of its throttled rounds, and
//   * the compositor delivered far fewer frames than a free-running pump would
//     have, because the pauses between the client's rounds cost nothing.
//
// Skips (77) without a Vulkan device.
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

import luminaria.gpu;

namespace {

constexpr int kOutW = 64, kOutH = 48;
constexpr int kWinW = 16, kWinH = 16;
// Twenty rounds, 20ms apart. A 1ms free-running pump would deliver ~400 frames
// across that; an on-demand one delivers a couple per round.
constexpr int kRounds = 20;
constexpr int kPauseMs = 20;
constexpr unsigned kIntervalMs = 1;

std::atomic<int> g_rounds_done{0};

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
};

wl_buffer* make_buffer(ClientState* st, int w, int h, std::uint32_t argb) {
    const int stride = w * 4;
    const int size = stride * h;
    int fd = memfd_create("luminaria-test", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, size) == 0);
    auto* px = static_cast<std::uint32_t*>(
        mmap(nullptr, static_cast<std::size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(px != MAP_FAILED);
    for (int i = 0; i < w * h; ++i) {
        px[i] = argb;
    }
    munmap(px, static_cast<std::size_t>(size));
    wl_shm_pool* pool = wl_shm_create_pool(st->shm, fd, size);
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_shm") == 0) {
        st->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

void on_frame_callback(void* data, wl_callback* cb, uint32_t) {
    *static_cast<bool*>(data) = true;
    wl_callback_destroy(cb);
}
const wl_callback_listener kFrameListener{on_frame_callback};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState st;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display);
    if (st.compositor == nullptr || st.shm == nullptr) {
        wl_display_disconnect(display);
        return;
    }

    wl_surface* surface = wl_compositor_create_surface(st.compositor);
    for (int round = 0; round < kRounds; ++round) {
        // Between rounds the client is asleep and the screen is still: this is
        // the pause the compositor must not spend flipping.
        if (round > 0) {
            const timespec pause{0, kPauseMs * 1000000L};
            nanosleep(&pause, nullptr);
        }
        bool done = false;
        wl_callback* cb = wl_surface_frame(surface);
        wl_callback_add_listener(cb, &kFrameListener, &done);
        // A different colour each round, so the frame really does have to be
        // repainted rather than being an unchanged one in disguise.
        wl_surface_attach(surface,
                          make_buffer(&st, kWinW, kWinH,
                                      0xFF000000u | static_cast<std::uint32_t>(round * 7 + 1)),
                          0, 0);
        wl_surface_damage(surface, 0, 0, kWinW, kWinH);
        wl_surface_commit(surface);
        wl_display_flush(display);
        // Block until the compositor says the frame landed. A compositor that
        // went idle and never woke up hangs here, and the test times out.
        while (!done) {
            if (wl_display_dispatch(display) < 0) {
                wl_display_disconnect(display);
                return;
            }
        }
        g_rounds_done.fetch_add(1, std::memory_order_relaxed);
    }
    wl_display_disconnect(display);
}

struct DestroyCtx {
    wl_listener listener;
    luminaria::Display* display;
};
void on_client_destroy(wl_listener* l, void*) {
    auto* ctx = reinterpret_cast<DestroyCtx*>(reinterpret_cast<char*>(l) -
                                              offsetof(DestroyCtx, listener));
    ctx->display->terminate();
}

std::uint32_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

} // namespace

int main() {
    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77;
    }

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    assert(display->init_shm().has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());

    luminaria::HeadlessOutput output(display->event_loop(), kOutW, kOutH, kIntervalMs);
    luminaria::Frame frame(output, *renderer);
    auto ready = frame.reset(DRM_FORMAT_XRGB8888);
    if (!ready) {
        std::fprintf(stderr, "skip: %s\n", ready.error().message.c_str());
        return 77;
    }

    luminaria::Surface* window = nullptr;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        if (window == nullptr) {
            window = &e.surface;
            // A surface nobody has drawn yet is not being watched by the frame
            // either, so this first wake-up is the compositor's to make.
            output.schedule_frame();
        }
    });

    int frames = 0;
    int unchanged = 0;
    const luminaria::Box view{0, 0, kOutW, kOutH};
    auto on_frame = output.frame.connect([&](luminaria::FrameEvent&) {
        ++frames;
        frame.begin(view);
        if (window != nullptr) {
            frame.place(*window, 0, 0);
        }
        auto presented = frame.submit(luminaria::Color{0, 0, 1, 1});
        assert(presented.has_value());
        if (*presented == luminaria::Presented::unchanged) {
            ++unchanged;
        }
        // Pace the clients. On `unchanged` no frame was committed and no
        // `present` will follow, so this is the only chance to do it — and
        // withholding the callback is exactly what would freeze the client.
        const std::uint32_t time = now_ms();
        for (const luminaria::Placement& p : frame.placements()) {
            if (luminaria::Surface* s = luminaria::surface_from_id(p.surface); s != nullptr) {
                s->send_frame_done(time);
            }
        }
    });

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

    // The client never stalled: every round got its frame callback.
    assert(g_rounds_done.load(std::memory_order_relaxed) == kRounds);
    // And the compositor was not spinning while it waited. The pauses alone
    // would have cost kRounds * kPauseMs / kIntervalMs frames on a free-running
    // pump; a generous fraction of that is still an order of magnitude fewer.
    const int free_running = kRounds * kPauseMs / static_cast<int>(kIntervalMs);
    assert(frames < free_running / 3);
    std::fprintf(stderr, "%d frames (%d unchanged) for %d rounds; a 1ms pump would have run %d\n",
                 frames, unchanged, kRounds, free_running);
    return 0;
}
