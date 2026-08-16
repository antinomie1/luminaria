// A client-supplied stride must be validated before we index client memory.
//
// libwayland's wl_shm_pool.create_buffer validation is `stride >= width`, which
// compares bytes against pixels: it has no idea a format is 4 bytes per pixel.
// So an ARGB8888 buffer with `stride == width` is accepted upstream and reaches
// us looking valid, while every row is a quarter of what the pixel loop walks.
//
// Before the stride check in Surface::current_buffer_rgba(), this test died
// with SIGSEGV: the pool is 1 MiB and the read wants 4 MiB, so the overrun
// leaves the mapping outright. (Note ASan does not catch this class — the
// overrun is past an mmap'd region, not a heap block — which is why the sizes
// here are chosen to fault rather than to be diagnosed.)
//
// The second case guards the other direction: a correctly strided buffer must
// still read back, so the fix cannot be "refuse everything".
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

// Sized so the short-stride overrun (4 MiB wanted vs 1 MiB mapped) leaves the
// mapping outright rather than landing in slack inside the last page.
constexpr int kWidth = 4096;
constexpr int kHeight = 256;
constexpr int kBadStride = kWidth;              // pixels, not bytes: 4x too small
constexpr int kPoolSize = kBadStride * kHeight; // 1 MiB; the read wants 4 MiB
constexpr int kGoodStride = kWidth * 4;
constexpr int kGoodPoolSize = kGoodStride * kHeight;

// Which buffer the client under test should send.
enum class Mode { ShortStride, CorrectStride };
Mode g_mode = Mode::ShortStride;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t /*version*/) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 1));
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
    if (st.compositor != nullptr && st.shm != nullptr) {
        const bool bad = g_mode == Mode::ShortStride;
        const int stride = bad ? kBadStride : kGoodStride;
        const int size = bad ? kPoolSize : kGoodPoolSize;

        const int pool_fd = ::memfd_create("stride-test", 0);
        assert(pool_fd >= 0);
        assert(::ftruncate(pool_fd, size) == 0);

        wl_shm_pool* pool = wl_shm_create_pool(st.shm, pool_fd, size);
        // In the bad case stride == width: a quarter of what ARGB8888 needs.
        wl_buffer* buffer =
            wl_shm_pool_create_buffer(pool, 0, kWidth, kHeight, stride, WL_SHM_FORMAT_ARGB8888);
        wl_surface* surface = wl_compositor_create_surface(st.compositor);
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_commit(surface);
        wl_display_roundtrip(display);
        ::close(pool_fd);
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

/// Drive one client against a fresh server. Returns what
/// Surface::current_buffer_rgba() decided about the committed buffer.
bool run_case(Mode mode, bool& commit_seen) {
    g_mode = mode;
    int fds[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    assert(display->init_shm());

    bool buffer_accepted = false;
    bool readback_returned = false;
    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;

    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        conns.push_back(e.surface.commit.connect([&](luminaria::SurfaceCommit& c) {
            buffer_accepted = true;
            // This is the read a real compositor performs every frame on the
            // headless / screencopy path.
            std::vector<std::uint8_t> pixels;
            int w = 0;
            int h = 0;
            readback_returned = c.surface.current_buffer_rgba(pixels, w, h);
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

    commit_seen = buffer_accepted;
    return readback_returned;
}

} // namespace

int main() {
    // libwayland lets the short-stride buffer through, so the commit must
    // arrive — and the readback must refuse it. Before the fix, SIGSEGV.
    bool commit_seen = false;
    const bool read_bad = run_case(Mode::ShortStride, commit_seen);
    assert(commit_seen);
    assert(!read_bad);

    // ...and a properly strided buffer must still be readable.
    commit_seen = false;
    const bool read_good = run_case(Mode::CorrectStride, commit_seen);
    assert(commit_seen);
    assert(read_good);
    return 0;
}
