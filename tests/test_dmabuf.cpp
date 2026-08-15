// End-to-end: a real client allocates a LINEAR dmabuf via GBM, fills it red,
// and hands it to the server through zwp_linux_dmabuf_v1 create_immed. The
// server imports it via Surface::current_buffer_rgba (mmap path) and composites.
// Skips (77) without a Vulkan device or a usable DRM render node.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gbm.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"

import luminaria.gpu;

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "wl_compositor") == 0) {
        st->compositor =
            static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(iface, "zwp_linux_dmabuf_v1") == 0) {
        st->dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 3));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

void run_client(int fd, int render_fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    gbm_device* gbm = gbm_create_device(render_fd);
    ClientState st;
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistry, &st);
    wl_display_roundtrip(display);

    if (gbm != nullptr && st.compositor != nullptr && st.dmabuf != nullptr) {
        const int w = 4, h = 4;
        gbm_bo* bo =
            gbm_bo_create(gbm, w, h, GBM_FORMAT_ARGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
        if (bo != nullptr) {
            uint32_t map_stride = 0;
            void* map_data = nullptr;
            void* map = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &map_stride, &map_data);
            if (map != nullptr && map != MAP_FAILED) {
                for (int y = 0; y < h; ++y) {
                    auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(map) +
                                                            static_cast<size_t>(y) * map_stride);
                    for (int x = 0; x < w; ++x) {
                        row[x] = 0xFFFF0000u; // ARGB: red, opaque
                    }
                }
                gbm_bo_unmap(bo, map_data);

                int bfd = gbm_bo_get_fd(bo);
                const uint32_t offset = gbm_bo_get_offset(bo, 0);
                const uint32_t stride = gbm_bo_get_stride(bo);
                const uint64_t mod = gbm_bo_get_modifier(bo);
                zwp_linux_buffer_params_v1* params =
                    zwp_linux_dmabuf_v1_create_params(st.dmabuf);
                zwp_linux_buffer_params_v1_add(params, bfd, 0, offset, stride,
                                               static_cast<uint32_t>(mod >> 32),
                                               static_cast<uint32_t>(mod));
                wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(
                    params, w, h, GBM_FORMAT_ARGB8888, 0);
                wl_surface* surface = wl_compositor_create_surface(st.compositor);
                wl_surface_attach(surface, buffer, 0, 0);
                wl_surface_commit(surface);
                wl_display_roundtrip(display);
                close(bfd);
            }
            gbm_bo_destroy(bo);
        }
    }
    if (gbm != nullptr) {
        gbm_device_destroy(gbm);
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
    int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (render_fd < 0) {
        std::fprintf(stderr, "skip: no /dev/dri/renderD128\n");
        return 77;
    }

    // Vulkan import/export roundtrip on whatever modifier the driver picks (tiled
    // included). Exercises the GPU path; the protocol test below covers LINEAR mmap.
    if (renderer->dmabuf_supported()) {
        gbm_device* g = gbm_create_device(render_fd);
        assert(g != nullptr);
        const int W = 4, H = 4;
        gbm_bo* bo = gbm_bo_create(g, W, H, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING);
        assert(bo != nullptr);
        const uint64_t mod = gbm_bo_get_modifier(bo);
        int bfd = gbm_bo_get_fd(bo);
        const uint32_t off = gbm_bo_get_offset(bo, 0);
        const uint32_t st = gbm_bo_get_stride(bo);

        std::vector<uint8_t> red(static_cast<size_t>(W) * H * 4);
        for (size_t i = 0; i < red.size(); i += 4) {
            red[i] = 255;
            red[i + 3] = 255;
        }
        assert(renderer->export_dmabuf(bfd, W, H, GBM_FORMAT_ARGB8888, off, st, mod, red)
                   .has_value());
        auto back = renderer->import_dmabuf(bfd, W, H, GBM_FORMAT_ARGB8888, off, st, mod);
        assert(back.has_value());
        assert((*back)[0] == 255 && (*back)[1] == 0 && (*back)[2] == 0 && (*back)[3] == 255);

        close(bfd);
        gbm_bo_destroy(bo);
        gbm_device_destroy(g);
        std::fprintf(stderr, "dmabuf vulkan roundtrip OK (modifier=0x%llx)\n",
                     static_cast<unsigned long long>(mod));
    }

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto dmabuf = luminaria::LinuxDmabuf::create(*display, &*renderer);
    if (!dmabuf) {
        std::fprintf(stderr, "skip: %s\n", dmabuf.error().message.c_str());
        return 77;
    }

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

    std::thread client_thread(run_client, fds[1], render_fd);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(3000);
    display->run();
    client_thread.join();
    close(render_fd);

    assert(g_ok);
    assert((g_frame[1 * 8 + 1] == luminaria::Pixel{255, 0, 0, 255})); // client red inside texture
    assert((g_frame[5 * 8 + 5] == luminaria::Pixel{0, 0, 0, 255}));   // background outside
    return 0;
}
