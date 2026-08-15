// End-to-end: a real client attaches a wl_shm buffer; the server reads it via
// Surface::current_buffer_rgba, composites it on the GPU, and we verify the
// client's pixels landed in the output. Skips (77) without a Vulkan device.
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

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
};

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

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState st;
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistry, &st);
    wl_display_roundtrip(display);

    if (st.compositor != nullptr && st.shm != nullptr) {
        const int w = 4, h = 4, stride = w * 4, size = stride * h;
        int bfd = memfd_create("cbuf", MFD_CLOEXEC);
        assert(ftruncate(bfd, size) == 0);
        auto* px = static_cast<uint32_t*>(
            mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0));
        std::fill_n(px, static_cast<size_t>(w) * h, 0xFFFF0000u); // ARGB: red, opaque
        munmap(px, size);
        wl_shm_pool* pool = wl_shm_create_pool(st.shm, bfd, size);
        wl_buffer* buffer =
            wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
        wl_surface* surface = wl_compositor_create_surface(st.compositor);
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_commit(surface);
        wl_display_roundtrip(display);
        wl_shm_pool_destroy(pool);
        close(bfd);
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

bool g_ok = false;
std::vector<luminaria::Pixel> g_frame;

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

    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* s = &e.surface;
        conns.push_back(e.surface.commit.connect([&, s](luminaria::SurfaceCommit&) {
            std::vector<std::uint8_t> rgba;
            int w = 0, h = 0;
            if (s->current_buffer_rgba(rgba, w, h)) {
                luminaria::TextureFill tex{0, 0, w, h, rgba.data()};
                auto frame = renderer->composite(8, 8, luminaria::Color{0, 0, 0, 1}, {}, {&tex, 1});
                if (frame) {
                    g_frame = std::move(*frame);
                    g_ok = true;
                }
            }
        }));
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx dc{{}, &*display};
    dc.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &dc.listener);

    std::thread client_thread(run_client, fds[1]);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(3000);
    display->run();
    client_thread.join();

    assert(g_ok);
    assert((g_frame[1 * 8 + 1] == luminaria::Pixel{255, 0, 0, 255})); // client red inside texture
    assert((g_frame[5 * 8 + 5] == luminaria::Pixel{0, 0, 0, 255}));   // background outside
    return 0;
}
