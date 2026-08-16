// What the placement list costs when it CHANGES.
//
// A window moving, opening, closing or changing depth is invisible to every
// client: nobody damages a surface because it was placed somewhere else. The
// compositor used to owe that bookkeeping by hand, and the only tool it had was
// `damage_all()` — a full-output repaint for a window that slid ten pixels.
//
// `Frame::submit()` now recovers it by diffing this frame's list against the
// last one it drew, so the test has to show two things at once: that the damage
// appears at all, and that it is MINIMAL. Both are read off the same trick —
// the background colour changes between frames, so any pixel that was repainted
// carries the new one and any pixel that was not still carries the old. A full
// repaint and a correct partial one are indistinguishable by the window alone.
//
// Skips (77) without a Vulkan device.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

import luminaria.gpu;
import std;

namespace {

constexpr int kOutW = 64, kOutH = 48;
constexpr int kWinW = 16, kWinH = 12;
// Where the window starts and where the compositor moves it to. The two
// rectangles must not touch, so that "only these two were repainted" is a
// statement about two separate places.
constexpr int kFromX = 2, kFromY = 2;
constexpr int kToX = 40, kToY = 30;

const luminaria::Pixel kRed{255, 0, 0, 255};
const luminaria::Pixel kBlue{0, 0, 255, 255};
const luminaria::Pixel kGreen{0, 255, 0, 255};

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
void registry_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_remove};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    assert(display != nullptr);
    ClientState st;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display);

    if (st.compositor != nullptr && st.shm != nullptr) {
        wl_surface* surface = wl_compositor_create_surface(st.compositor);
        wl_surface_attach(surface, make_buffer(&st, kWinW, kWinH, 0xFFFF0000u), 0, 0);
        wl_surface_damage(surface, 0, 0, kWinW, kWinH);
        wl_surface_commit(surface);
        wl_display_roundtrip(display); // the server does its checking in here
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

} // namespace

int main() {
    // Before the Display: the frame caches textures owned by this renderer.
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

    luminaria::HeadlessOutput output(display->event_loop(), kOutW, kOutH, 16);
    luminaria::Frame frame(output, *renderer);
    auto ready = frame.reset(DRM_FORMAT_XRGB8888);
    if (!ready) {
        std::fprintf(stderr, "skip: %s\n", ready.error().message.c_str());
        return 77;
    }

    const luminaria::Box view{0, 0, kOutW, kOutH};
    bool checked = false;
    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;

    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* surface = &e.surface;
        conns.push_back(surface->commit.connect([&, surface](luminaria::SurfaceCommit&) {
            if (checked) {
                return;
            }
            checked = true;

            auto at = [&](int x, int y) {
                const std::vector<luminaria::Pixel>& px = output.last_frame();
                return px[static_cast<std::size_t>(y) * kOutW + x];
            };

            // --- frame 1: the window where it starts, on blue ----------------
            frame.begin(view);
            frame.place(*surface, kFromX, kFromY);
            auto first = frame.submit(luminaria::Color{0, 0, 1, 1});
            assert(first.has_value());
            assert(*first == luminaria::Presented::composited);
            assert(at(kFromX + 1, kFromY + 1) == kRed);
            assert(at(kToX + 1, kToY + 1) == kBlue);
            assert(at(kOutW - 1, kOutH - 1) == kBlue);

            // --- frame 2: nothing moved --------------------------------------
            // The diff finds no difference and the client committed nothing, so
            // there is not one stale pixel and the output goes idle.
            frame.begin(view);
            frame.place(*surface, kFromX, kFromY);
            auto same = frame.submit(luminaria::Color{0, 0, 1, 1});
            assert(same.has_value());
            assert(*same == luminaria::Presented::unchanged);

            // --- frame 3: the compositor moves it ----------------------------
            // No client commit, no damage_all(), and the background changes to
            // green so the repainted area can be read straight off the pixels.
            frame.invalidate();
            assert(output.frame_scheduled());
            frame.begin(view);
            frame.place(*surface, kToX, kToY);
            auto moved = frame.submit(luminaria::Color{0, 1, 0, 1});
            assert(moved.has_value());
            assert(*moved == luminaria::Presented::composited);

            // The window is at its new home, drawn over the new background.
            assert(at(kToX + 1, kToY + 1) == kRed);
            // Where it was is repainted — the old rectangle really was damaged,
            // or the window would still be sitting there.
            assert(at(kFromX + 1, kFromY + 1) == kGreen);
            // And nowhere else was touched. This is the whole point: a full
            // repaint would have made this green too. It is still the blue from
            // frame 1, which is exactly what "minimal damage" means.
            assert(at(kOutW - 1, kOutH - 1) == kBlue);
            assert(at(kOutW / 2, 1) == kBlue);

            // --- frame 4: the compositor takes it away ------------------------
            // An unplaced window is as invisible to client damage as a moved
            // one, and the diff has to notice it left.
            frame.invalidate();
            frame.begin(view);
            auto gone = frame.submit(luminaria::Color{0, 1, 0, 1});
            assert(gone.has_value());
            assert(*gone == luminaria::Presented::composited);
            assert(at(kToX + 1, kToY + 1) == kGreen);
            // Still untouched, three frames running.
            assert(at(kOutW - 1, kOutH - 1) == kBlue);

            // --- frame 5: and once more, nothing ------------------------------
            frame.begin(view);
            auto empty = frame.submit(luminaria::Color{0, 1, 0, 1});
            assert(empty.has_value());
            assert(*empty == luminaria::Presented::unchanged);
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

    assert(checked);
    return 0;
}
