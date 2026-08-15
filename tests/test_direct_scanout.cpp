// Direct scanout: putting a client's buffer on the screen instead of drawing.
//
// Two halves, and the second is the one with teeth.
//
// `DirectScanout` decides whether a surface may bypass compositing at all. On a
// headless output the answer is always no, and it has to be a CHEAP no —
// a client that hands us an unscannable buffer every frame must not cost a
// failed import every frame.
//
// The other half is what makes direct scanout safe: while the display hardware
// is reading a client's buffer, the client must not be told it may draw into it
// again. Normally `wl_buffer.release` goes out the moment the next buffer is
// committed; `Surface::hold_buffer` defers exactly that, and the release has to
// arrive later rather than never. A real client on a socketpair watches for it.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

import luminaria.gpu;

namespace {

constexpr int kW = 4, kH = 4;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    // Which of our three buffers the server has handed back, in order.
    std::vector<int> released;
    // A snapshot taken right after the second commit: the held buffer must NOT
    // be in there yet.
    std::size_t released_after_second = 0;
    std::size_t released_after_third = 0;
};

ClientState g_client;
wl_buffer* g_buffers[3] = {nullptr, nullptr, nullptr};

void on_release(void*, wl_buffer* buffer) {
    for (int i = 0; i < 3; ++i) {
        if (g_buffers[i] == buffer) {
            g_client.released.push_back(i);
        }
    }
}
const wl_buffer_listener kBufferListener{on_release};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "wl_compositor") == 0) {
        st->compositor =
            static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(iface, "wl_shm") == 0) {
        st->shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

wl_buffer* make_buffer(wl_shm* shm, std::uint32_t argb) {
    const int stride = kW * 4, size = stride * kH;
    int fd = memfd_create("dsbuf", MFD_CLOEXEC);
    assert(ftruncate(fd, size) == 0);
    auto* px =
        static_cast<std::uint32_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    std::fill_n(px, static_cast<std::size_t>(kW) * kH, argb);
    munmap(px, size);
    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    wl_buffer* buffer =
        wl_shm_pool_create_buffer(pool, 0, kW, kH, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(buffer, &kBufferListener, nullptr);
    return buffer;
}

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState& st = g_client;
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistry, &st);
    wl_display_roundtrip(display);
    if (st.compositor == nullptr || st.shm == nullptr) {
        wl_display_disconnect(display);
        return;
    }

    for (int i = 0; i < 3; ++i) {
        g_buffers[i] = make_buffer(st.shm, 0xFF000000u | static_cast<std::uint32_t>(i));
    }
    wl_surface* surface = wl_compositor_create_surface(st.compositor);

    // Commit 0: the server holds this one, as a direct scanout would.
    wl_surface_attach(surface, g_buffers[0], 0, 0);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    // Commit 1 replaces it. Buffer 0 is "on screen", so no release yet.
    wl_surface_attach(surface, g_buffers[1], 0, 0);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); // a release, if one were coming, would be here
    st.released_after_second = st.released.size();

    // Commit 2 replaces buffer 1, which nobody held: that release is immediate.
    wl_surface_attach(surface, g_buffers[2], 0, 0);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    st.released_after_third = st.released.size();

    // Sit on the socket: the deferred release for buffer 0 is still to come.
    while (wl_display_dispatch(display) >= 0) {
    }
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
    assert(display->init_shm().has_value());

    luminaria::HeadlessBackend backend(display->event_loop(), /*frame_interval_ms=*/1000);
    luminaria::Output& output = backend.add_output(kW, kH);

    // --- the policy half ---
    luminaria::DirectScanout direct{output};
    // Headless scans out nothing, so it must not claim it can. This is what
    // stops a compositor allocating a target the output would then refuse.
    assert(!direct.available());
    assert(direct.cached() == 0);

    luminaria::Surface* surface = nullptr;
    int commits = 0;
    wl_resource* held = nullptr;
    luminaria::Signal<luminaria::SurfaceCommit>::Connection commit_conn;

    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        surface = &e.surface;
        commit_conn = e.surface.commit.connect([&](luminaria::SurfaceCommit& ce) {
            ++commits;
            if (commits == 1) {
                // Stand in for "this buffer is now scanning out".
                held = ce.surface.current_buffer();
                assert(held != nullptr);
                ce.surface.hold_buffer(held);
                // An shm buffer can never be scanned out, and asking twice must
                // not cost two attempts — the refusal is cached with the rest.
                assert(!direct.id_for(ce.surface).has_value());
                assert(direct.cached() == 1);
                assert(!direct.id_for(ce.surface).has_value());
                assert(direct.cached() == 1);
            }
        });
    });

    // Release the hold once the client has had time to observe that it did NOT
    // get its buffer back. The client is still dispatching, so the deferred
    // release reaches it.
    bool unheld = false;
    auto unhold = display->event_loop().add_timer([&] {
        assert(surface != nullptr && held != nullptr);
        surface->unhold_buffer(held);
        unheld = true;
    });
    unhold.arm(250);

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_client, fds[1]);
    auto stop = display->event_loop().add_timer([&] { display->terminate(); });
    stop.arm(600);
    display->run();
    wl_client_destroy(client);
    client_thread.join();

    assert(commits == 3);
    assert(unheld);
    // Buffer 0 was held, so replacing it released nothing...
    assert(g_client.released_after_second == 0);
    // ...while buffer 1, held by nobody, came back the moment it was replaced.
    assert(g_client.released_after_third == 1);
    assert(g_client.released.front() == 1);
    // The deferred release did arrive, exactly once, after the hold was
    // dropped. Withholding it forever would freeze a client's swapchain.
    assert(g_client.released.size() == 2);
    assert(g_client.released.back() == 0);
    return 0;
}
