// The generic CPU compositor: a real client (over a socketpair) commits a
// 4x4 red surface with a 2x2 blue subsurface above it; the server composites
// the tree at (2,2) over a solid white rectangle and a green background, and
// asserts the pixels — background where nothing is, white where only the rect
// is, red where the tree is, blue where the subsurface sits on top. Then it
// releases both clients' frame callbacks through the batch helper and the
// client asserts it got exactly two `done` events at the same stamp.
//
// No GPU anywhere: the compositing is the CPU path a headless or nested
// compositor (or a test harness) runs instead of Frame.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-server-core.h>

import luminaria;
import std;

namespace {

constexpr int kWidth = 10;
constexpr int kHeight = 10;
constexpr std::uint32_t kDoneStamp = 1234;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_shm* shm = nullptr;
};

wl_buffer* make_color_buffer(ClientState* st, int w, int h, std::uint8_t r, std::uint8_t g,
                             std::uint8_t b, std::uint8_t a) {
    const int stride = w * 4;
    const int size = stride * h;
    int fd = memfd_create("luminaria-cpu", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, size) == 0);
    auto* data = static_cast<std::uint8_t*>(
        mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(data != MAP_FAILED);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = data + (static_cast<std::size_t>(y) * stride + x * 4);
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = a; // ARGB8888 is little-endian BGRA bytes
        }
    }
    munmap(data, size);
    wl_shm_pool* pool = wl_shm_create_pool(st->shm, fd, size);
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

// The frame callbacks the client asked for: exactly two `done` events must
// come back, both stamped with the compositor's one time.
struct CbState {
    int done = 0;
    std::uint32_t times[2] = {0, 0};
};
CbState g_cb;

void cb_done(void* data, wl_callback* cb, std::uint32_t time) {
    auto* s = static_cast<CbState*>(data);
    if (s->done < 2) {
        s->times[s->done] = time;
    }
    ++s->done;
    wl_callback_destroy(cb);
}
const wl_callback_listener kCbListener{cb_done};

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
        wl_subsurface_set_position(sub, 1, 1);
        wl_subsurface_place_above(sub, parent);

        // Both clients want pacing: neither may be left waiting for a callback.
        wl_callback* cb_parent = wl_surface_frame(parent);
        wl_callback* cb_child = wl_surface_frame(child);
        wl_callback_add_listener(cb_parent, &kCbListener, &g_cb);
        wl_callback_add_listener(cb_child, &kCbListener, &g_cb);

        wl_surface_attach(child, make_color_buffer(&st, 2, 2, 0, 0, 255, 255), 0, 0);
        wl_surface_commit(child); // sync mode (the default): cached...
        wl_surface_attach(parent, make_color_buffer(&st, 4, 4, 255, 0, 0, 255), 0, 0);
        wl_surface_commit(parent); // ...until the parent applies the whole tree
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

int main() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    assert(display->init_shm().has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto subcompositor = luminaria::Subcompositor::create(*display);
    assert(subcompositor.has_value());

    std::vector<luminaria::Surface*> surfaces; // creation order: parent, child
    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> commit_conns;
    bool composited = false;
    bool released = false;

    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        surfaces.push_back(&e.surface);
        if (surfaces.size() != 1) {
            return; // composite once the parent (the first surface) commits
        }
        commit_conns.push_back(e.surface.commit.connect([&](luminaria::SurfaceCommit&) {
            // Frame callbacks are queued only after the whole commit dispatch
            // (the tree applies atomically), so run this on the next idle turn
            // of the loop — by then both buffers are in and both callbacks are
            // queued, exactly like a present handler would see them.
            display->event_loop().once([&] {
                if (composited) {
                    return;
                }
                composited = true;

                // One draw list: a compositor-owned solid rectangle behind the
                // client's surface tree at (2,2), over a green background.
                luminaria::CpuCompositor cpu;
                std::vector<luminaria::CpuItem> items;
                items.push_back(luminaria::RectFill{{1, 1, 4, 4}, luminaria::Color{1, 1, 1, 1}});
                items.push_back(luminaria::CpuView{surfaces[0]->id(), 2, 2});
                cpu.composite(kWidth, kHeight, luminaria::Color{0, 1, 0, 1}, items);

                const auto px = [&](int x, int y) {
                    return cpu.pixels()[static_cast<std::size_t>(y) * kWidth + x];
                };
                // Where nothing is drawn: the background.
                assert(px(0, 0) == (luminaria::Pixel{0, 255, 0, 255}));
                assert(px(6, 6) == (luminaria::Pixel{0, 255, 0, 255}));
                // Where only the solid rectangle is.
                assert(px(1, 1) == (luminaria::Pixel{255, 255, 255, 255}));
                // The tree's parent surface (red 4x4) and its subsurface (blue
                // 2x2 at +1,+1, stacked above): the child wins where they
                // overlap.
                assert(px(2, 2) == (luminaria::Pixel{255, 0, 0, 255}));
                assert(px(3, 3) == (luminaria::Pixel{0, 0, 255, 255}));
                assert(px(4, 4) == (luminaria::Pixel{0, 0, 255, 255}));
                assert(px(5, 5) == (luminaria::Pixel{255, 0, 0, 255}));

                // A view clip confines the whole tree: everything it would
                // paint outside the box must leave the underlying pixels byte
                // for byte — the tree's own corner stays white because the
                // clip (not the tree) decides where the red may land.
                std::vector<luminaria::CpuItem> clipped_items;
                clipped_items.push_back(
                    luminaria::RectFill{{1, 1, 4, 4}, luminaria::Color{1, 1, 1, 1}});
                // Tree at (2,2) — red 4x4, blue 2x2 at +1,+1 — clipped to the
                // device box (3,3)-(6,6): only its bottom-right 3x3 may draw.
                clipped_items.push_back(luminaria::CpuView{surfaces[0]->id(), 2, 2,
                                                           luminaria::Box{3, 3, 3, 3}});
                luminaria::CpuCompositor cpu2;
                cpu2.composite(kWidth, kHeight, luminaria::Color{0, 1, 0, 1}, clipped_items);
                const auto cpx = [&](int x, int y) {
                    return cpu2.pixels()[static_cast<std::size_t>(y) * kWidth + x];
                };
                // Outside the clip the tree never painted: the red corner that
                // would cover (2,2) is absent, and the white rect underneath is
                // untouched. (2,5)/(5,2) sit outside both the clip and the
                // rect, so the red the tree would have put there never landed
                // and the background stayed.
                assert(cpx(2, 2) == (luminaria::Pixel{255, 255, 255, 255}));
                assert(cpx(2, 5) == (luminaria::Pixel{0, 255, 0, 255}));
                assert(cpx(5, 2) == (luminaria::Pixel{0, 255, 0, 255}));
                // Outside both the clip and the rect: the background.
                assert(cpx(6, 6) == (luminaria::Pixel{0, 255, 0, 255}));
                // Inside the clip the tree draws exactly as unclipped: the
                // subsurface still wins over its parent where they overlap.
                assert(cpx(3, 3) == (luminaria::Pixel{0, 0, 255, 255}));
                assert(cpx(4, 4) == (luminaria::Pixel{0, 0, 255, 255}));
                assert(cpx(5, 5) == (luminaria::Pixel{255, 0, 0, 255}));

                // A clip that pokes past the framebuffer edge is clamped to it:
                // tree at (9,9) would paint a 4x4, but only (9,9) is on screen.
                std::vector<luminaria::CpuItem> edge_items;
                edge_items.push_back(luminaria::CpuView{surfaces[0]->id(), 9, 9,
                                                        luminaria::Box{8, 8, 10, 10}});
                luminaria::CpuCompositor cpu3;
                cpu3.composite(kWidth, kHeight, luminaria::Color{0, 1, 0, 1}, edge_items);
                const auto epx = [&](int x, int y) {
                    return cpu3.pixels()[static_cast<std::size_t>(y) * kWidth + x];
                };
                assert(epx(9, 9) == (luminaria::Pixel{255, 0, 0, 255}));
                assert(epx(8, 8) == (luminaria::Pixel{0, 255, 0, 255}));
                assert(epx(9, 8) == (luminaria::Pixel{0, 255, 0, 255}));
                assert(epx(8, 9) == (luminaria::Pixel{0, 255, 0, 255}));

                // Batch frame-callback release: one stamp for the whole frame.
                const std::array<luminaria::SurfaceId, 2> ids{surfaces[0]->id(),
                                                              surfaces[1]->id()};
                luminaria::send_frame_done(ids, kDoneStamp);
                released = true;
            });
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

    assert(composited);
    assert(released);
    assert(g_cb.done == 2);
    assert(g_cb.times[0] == kDoneStamp);
    assert(g_cb.times[1] == kDoneStamp);
    return 0;
}
