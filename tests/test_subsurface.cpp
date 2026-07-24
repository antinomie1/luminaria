// wl_subcompositor: a child surface positioned under a parent, in the protocol's
// default SYNC mode. The client commits the child first (which must be cached,
// not applied), then commits the parent (which must apply the whole subtree
// atomically). Then it switches to desync and checks the child applies on its
// own. The server asserts the tree layout and the commit ordering.
#include <cassert>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"
#include "luminaria/subcompositor.hpp"

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_shm* shm = nullptr;
};

wl_buffer* make_buffer(ClientState* st, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    int fd = memfd_create("luminaria-test", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, size) == 0);
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
        wl_subsurface_set_position(sub, 10, 20);
        wl_display_roundtrip(display); // server sees both surfaces + the link

        // Sync mode (the default): the child's commit must be CACHED.
        wl_surface_attach(child, make_buffer(&st, 32, 16), 0, 0);
        wl_surface_commit(child);
        wl_display_roundtrip(display);

        // The parent's commit applies the child's cached state atomically.
        wl_surface_attach(parent, make_buffer(&st, 64, 64), 0, 0);
        wl_surface_commit(parent);
        wl_display_roundtrip(display);

        // Desync: from here the child applies its own commits.
        wl_subsurface_set_desync(sub);
        wl_surface_attach(child, make_buffer(&st, 8, 8), 0, 0);
        wl_surface_commit(child);
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

    luminaria::Surface* parent = nullptr;
    luminaria::Surface* child = nullptr;
    int parent_commits = 0;
    int child_commits = 0;
    int child_commits_when_parent_committed = -1;
    std::vector<luminaria::SurfaceAt> tree_at_child_commit;
    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;

    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* surface = &e.surface;
        if (parent == nullptr) {
            parent = surface;
        } else if (child == nullptr) {
            child = surface;
        }
        conns.push_back(surface->commit.connect([&, surface](luminaria::SurfaceCommit&) {
            if (surface == parent) {
                ++parent_commits;
                // The child's cached state must NOT have been applied yet.
                child_commits_when_parent_committed = child_commits;
            } else if (surface == child) {
                ++child_commits;
                if (parent != nullptr) {
                    tree_at_child_commit = parent->surface_tree();
                }
            }
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

    assert(parent != nullptr);
    assert(child != nullptr);
    // One parent commit; the child committed twice (once via the parent, once
    // after going desync).
    assert(parent_commits == 1);
    assert(child_commits == 2);
    // Sync mode really deferred the first child commit.
    assert(child_commits_when_parent_committed == 0);

    // The tree seen from the parent: child below-to-above ordering puts the
    // child on top, at its subsurface offset.
    assert(tree_at_child_commit.size() == 2);
    assert(tree_at_child_commit[0].surface == parent);
    assert(tree_at_child_commit[0].x == 0 && tree_at_child_commit[0].y == 0);
    assert(tree_at_child_commit[1].surface == child);
    assert(tree_at_child_commit[1].x == 10 && tree_at_child_commit[1].y == 20);
    return 0;
}
