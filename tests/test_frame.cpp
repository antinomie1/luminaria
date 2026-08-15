// The shell layer: a Frame's placement list, what it hit-tests to, what it
// draws, and what it costs.
//
// The last one is the point. Immediate mode is only affordable if refilling the
// list is free, so this counts heap allocations across a rebuild in the steady
// state and requires the count to be zero — every buffer the frame uses is
// cleared and refilled, never freed and regrown. It replaces global operator
// new to do that; the client runs in another thread but is blocked inside a
// roundtrip whenever a server-side handler is running, which is where the
// measurement happens.
//
// Skips (77) without a Vulkan device.
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

import luminaria;

namespace {

std::atomic<std::size_t> g_allocs{0};

constexpr int kOutW = 64, kOutH = 48;
constexpr int kParentW = 32, kParentH = 16;
constexpr int kChildW = 8, kChildH = 8;
// Where the compositor decides to put the window, and where the subsurface sits
// inside it.
constexpr int kWinX = 5, kWinY = 7;
constexpr int kSubX = 10, kSubY = 20;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
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
    } else if (std::strcmp(interface, "wl_subcompositor") == 0) {
        st->subcompositor = static_cast<wl_subcompositor*>(
            wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (std::strcmp(interface, "wl_shm") == 0) {
        st->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
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
    wl_display_roundtrip(display);

    if (st.compositor != nullptr && st.subcompositor != nullptr && st.shm != nullptr) {
        wl_surface* parent = wl_compositor_create_surface(st.compositor);
        wl_surface* child = wl_compositor_create_surface(st.compositor);
        wl_subsurface* sub = wl_subcompositor_get_subsurface(st.subcompositor, child, parent);
        wl_subsurface_set_position(sub, kSubX, kSubY);
        wl_subsurface_set_desync(sub);

        // The left half of the parent is opaque, and only the left half takes
        // input — two different regions, so neither can stand in for the other.
        wl_region* opaque = wl_compositor_create_region(st.compositor);
        wl_region_add(opaque, 0, 0, kParentW / 2, kParentH);
        wl_surface_set_opaque_region(parent, opaque);
        wl_region_destroy(opaque);
        wl_region* input = wl_compositor_create_region(st.compositor);
        wl_region_add(input, 0, 0, kParentW / 2, kParentH);
        wl_surface_set_input_region(parent, input);
        wl_region_destroy(input);

        wl_surface_attach(child, make_buffer(&st, kChildW, kChildH, 0xFF00FF00u), 0, 0);
        wl_surface_damage(child, 0, 0, kChildW, kChildH);
        wl_surface_commit(child);
        wl_surface_attach(parent, make_buffer(&st, kParentW, kParentH, 0xFFFF0000u), 0, 0);
        wl_surface_damage(parent, 0, 0, kParentW, kParentH);
        wl_surface_commit(parent);
        wl_display_roundtrip(display);
        // A subsurface's position lands on its parent's NEXT commit, so the
        // tree is only fully placed after this one. That is where the server
        // runs its checks.
        wl_surface_damage(parent, 0, 0, kParentW, kParentH);
        wl_surface_commit(parent);
        wl_display_roundtrip(display);
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

void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main() {
    // Before the Display: surfaces cache textures owned by this renderer.
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
    auto subcompositor = luminaria::Subcompositor::create(*display);
    assert(subcompositor.has_value());

    luminaria::HeadlessOutput output(display->event_loop(), kOutW, kOutH, 16);
    luminaria::Frame frame(output, *renderer);
    auto ready = frame.reset(DRM_FORMAT_XRGB8888);
    if (!ready) {
        std::fprintf(stderr, "skip: %s\n", ready.error().message.c_str());
        return 77;
    }
    // A headless output scans nothing out: one target, composited and read back,
    // and no client buffer can go straight to a display that isn't there.
    assert(frame.target_count() == 1);
    assert(!frame.direct_scanout_available());

    const luminaria::Box view{0, 0, kOutW, kOutH};
    luminaria::Surface* parent = nullptr;
    int parent_commits = 0;
    bool checked = false;

    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* surface = &e.surface;
        if (parent == nullptr) {
            parent = surface;
        }
        conns.push_back(surface->commit.connect([&, surface](luminaria::SurfaceCommit&) {
            if (surface != parent || checked) {
                return; // the parent's commit is the one that completes the tree
            }
            if (++parent_commits < 2) {
                return; // the subsurface's position lands on the next one
            }
            checked = true;
            frame.begin(view);
            frame.place(*parent, kWinX, kWinY);
            const std::span<const luminaria::Placement> list = frame.placements();
            assert(list.size() == 2);

            // --- the list ---------------------------------------------------
            // Back-to-front, the whole tree, in layout coordinates.
            assert(list[0].surface == parent);
            assert(list[0].x == kWinX && list[0].y == kWinY);
            assert(list[0].width == kParentW && list[0].height == kParentH);
            assert(list[1].x == kWinX + kSubX && list[1].y == kWinY + kSubY);
            assert(list[1].width == kChildW && list[1].height == kChildH);

            // The opaque region travels in layout coordinates, and it is the
            // client's region rather than the surface rectangle.
            const std::span<const luminaria::Box> opaque = frame.opaque_of(list[0]);
            assert(opaque.size() == 1);
            assert((opaque[0] == luminaria::Box{kWinX, kWinY, kParentW / 2, kParentH}));

            // --- hit-testing ------------------------------------------------
            double sx = -1, sy = -1;
            assert(frame.surface_at(kWinX + 1, kWinY + 1, sx, sy) == parent);
            assert(sx == 1 && sy == 1);
            // Inside the parent's rectangle but outside its input region: the
            // client says it does not want that half, so nothing is hit.
            assert(frame.surface_at(kWinX + kParentW - 1, kWinY + 1, sx, sy) == nullptr);
            // The subsurface is hit-testable in its own right, above the parent.
            assert(frame.surface_at(kWinX + kSubX + 1, kWinY + kSubY + 1, sx, sy) == list[1].surface);
            assert(sx == 1 && sy == 1);
            assert(frame.surface_at(kOutW - 1, kOutH - 1, sx, sy) == nullptr);

            // --- view culling -----------------------------------------------
            // A window on another monitor contributes nothing to this frame.
            frame.begin(view);
            frame.place(*parent, kOutW + 100, kOutH + 100);
            assert(frame.placements().empty());

            // --- steady-state cost ------------------------------------------
            // Rebuilding the list must allocate nothing at all: the vectors are
            // cleared, not freed. One warm-up round to let them reach their
            // size, then the count has to stay put.
            for (int i = 0; i < 4; ++i) {
                frame.begin(view);
                frame.place(*parent, kWinX, kWinY);
            }
            const std::size_t before = g_allocs.load(std::memory_order_relaxed);
            assert(before > 0); // the operator new replacement really is live
            for (int i = 0; i < 16; ++i) {
                frame.begin(view);
                frame.place(*parent, kWinX, kWinY);
                double hx = 0, hy = 0;
                (void)frame.surface_at(kWinX + 1, kWinY + 1, hx, hy);
            }
            assert(g_allocs.load(std::memory_order_relaxed) == before);

            // --- drawing ----------------------------------------------------
            frame.begin(view);
            frame.place(*parent, kWinX, kWinY);
            auto presented = frame.submit(luminaria::Color{0, 0, 1, 1});
            assert(presented.has_value());
            assert(*presented == luminaria::Presented::composited);

            const std::vector<luminaria::Pixel>& px = output.last_frame();
            assert(px.size() == static_cast<std::size_t>(kOutW) * kOutH);
            auto at = [&](int x, int y) { return px[static_cast<std::size_t>(y) * kOutW + x]; };
            assert((at(kWinX + 1, kWinY + 1) == luminaria::Pixel{255, 0, 0, 255}));
            assert((at(kWinX + kSubX + 1, kWinY + kSubY + 1) ==
                    luminaria::Pixel{0, 255, 0, 255}));
            assert((at(kOutW - 1, kOutH - 1) == luminaria::Pixel{0, 0, 255, 255}));

            // Read-back answers with the same frame, without drawing again.
            auto shown = frame.read_back();
            assert(shown.has_value());
            assert(shown->size() == px.size());
            assert(*(shown->data() + (kWinY + 1) * kOutW + kWinX + 1) ==
                   (luminaria::Pixel{255, 0, 0, 255}));

            // --- nothing changed --------------------------------------------
            // submit() consumed the clients' damage, so a second frame with no
            // new commits has nothing to repaint and says so. That answer is
            // what a compositor needs to stop flipping at 60Hz over a still
            // screen.
            frame.begin(view);
            frame.place(*parent, kWinX, kWinY);
            auto again = frame.submit(luminaria::Color{0, 0, 1, 1});
            assert(again.has_value());
            assert(*again == luminaria::Presented::unchanged);

            // Until something declares the whole output stale.
            frame.damage_all();
            frame.begin(view);
            frame.place(*parent, kWinX, kWinY);
            auto forced = frame.submit(luminaria::Color{0, 0, 1, 1});
            assert(forced.has_value());
            assert(*forced == luminaria::Presented::composited);
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
