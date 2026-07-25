// wl_surface state that used to be accepted and thrown away: buffer scale,
// buffer transform, input/opaque regions, damage_buffer and offset. Each one
// changes an answer the compositor gives — the surface size, where a click
// lands, or which pixels get repainted.
#include <cassert>
#include <cstddef>
#include <cstring>
#include <thread>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface,
                     uint32_t version) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "wl_compositor") == 0) {
        // Version 6 is what we advertise; the test needs at least 5 for offset.
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, version));
    } else if (std::strcmp(iface, "wl_shm") == 0) {
        st->shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

wl_buffer* make_buffer(wl_shm* shm, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    int fd = memfd_create("lum-test", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, size) == 0);
    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState st;
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistry, &st);
    wl_display_roundtrip(display);
    assert(st.compositor != nullptr && st.shm != nullptr);
    assert(wl_compositor_get_version(st.compositor) >= 5);

    wl_surface* surface = wl_compositor_create_surface(st.compositor);

    // A 2x HiDPI buffer stored rotated 90 degrees: 40x80 pixels becomes a
    // 40x20 surface (80/2 wide after the axis swap, 40/2 tall).
    wl_buffer* buffer = make_buffer(st.shm, 40, 80);
    wl_surface_set_buffer_scale(surface, 2);
    wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_90);

    // Input only in the left half of the surface.
    wl_region* input = wl_compositor_create_region(st.compositor);
    wl_region_add(input, 0, 0, 40, 20);
    wl_region_subtract(input, 20, 0, 20, 20);
    wl_surface_set_input_region(surface, input);
    wl_region_destroy(input);

    // Opaque everywhere but a 4x4 hole.
    wl_region* opaque = wl_compositor_create_region(st.compositor);
    wl_region_add(opaque, 0, 0, 40, 20);
    wl_region_subtract(opaque, 0, 0, 4, 4);
    wl_surface_set_opaque_region(surface, opaque);
    wl_region_destroy(opaque);

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface); // first buffer: everything is damaged
    wl_display_roundtrip(display);

    // Second commit: damage in BUFFER pixels only, so the server has to undo
    // the scale and the rotation to know what actually changed.
    wl_surface_damage_buffer(surface, 0, 0, 8, 8);
    wl_surface_offset(surface, 3, -2);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    wl_buffer_destroy(buffer);
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
    assert(wl_display_init_shm(display->c_ptr()) == 0);
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());

    int commits = 0;
    luminaria::Signal<luminaria::SurfaceCommit>::Connection commit_conn;
    auto new_surface = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        commit_conn = e.surface.commit.connect([&](luminaria::SurfaceCommit& ce) {
            luminaria::Surface& s = ce.surface;
            ++commits;
            // The buffer is 40x80 pixels; the surface is 40x20.
            assert(s.buffer_width() == 40 && s.buffer_height() == 80);
            assert(s.buffer_scale() == 2);
            assert(s.surface_width() == 40 && s.surface_height() == 20);

            // Input region: left half only, in surface coordinates.
            assert(s.has_input_region());
            assert(s.accepts_input(5, 5));
            assert(!s.accepts_input(25, 5));
            assert(!s.accepts_input(-1, 5));   // outside the surface entirely
            assert(!s.accepts_input(5, 100));

            // Opaque region survived the clip to the surface.
            assert(!s.opaque_region().contains(1, 1));
            assert(s.opaque_region().contains(10, 10));

            if (commits == 1) {
                // First buffer of a new size: all of it.
                assert(s.damage().size() == 1);
                assert((s.damage()[0] == luminaria::Box{0, 0, 40, 20}));
                assert(s.offset_x() == 0 && s.offset_y() == 0);
                s.clear_damage();
                return;
            }
            // damage_buffer(0,0,8,8) on a 40x80 buffer with scale 2 and a 90
            // degree rotation: 4x4 in surface units, and NOT at the origin —
            // undoing the rotation puts the buffer's top-left corner at the
            // surface's bottom-left.
            assert(s.damage().size() == 1);
            const luminaria::Box d = s.damage()[0];
            assert((d == luminaria::Box{0, 16, 4, 4}));
            assert(s.offset_x() == 3 && s.offset_y() == -2);
            // Offsets show up in the surface tree, which is what the renderer
            // and hit-testing both walk.
            const auto tree = s.surface_tree();
            assert(tree.size() == 1 && tree[0].x == 3 && tree[0].y == -2);
        });
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
    assert(commits == 2);
    return 0;
}
