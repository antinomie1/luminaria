// ext_session_lock_v1. The security property is the whole point, so that is
// what this drives: the compositor grants a lock, the client gets a surface
// sized to an output, and the session-locked latch flips only on an ORDERLY
// unlock. A second lock while one is held is refused rather than queued.
#include <cassert>
#include <cstdint>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "ext-session-lock-v1-client-protocol.h"

import luminaria.desktop;
import std;

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_output* output = nullptr;
    ext_session_lock_manager_v1* manager = nullptr;
    ext_session_lock_v1* lock = nullptr;
    bool locked = false;
    bool finished = false;
    int second_finished = 0;
    std::uint32_t configure_serial = 0;
    std::uint32_t configure_width = 0;
    std::uint32_t configure_height = 0;
};

ClientState g_client;

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_output") == 0) {
        st->output =
            static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, 2));
    } else if (std::strcmp(interface, "ext_session_lock_manager_v1") == 0) {
        st->manager = static_cast<ext_session_lock_manager_v1*>(
            wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

void lock_locked(void* data, ext_session_lock_v1*) {
    static_cast<ClientState*>(data)->locked = true;
}
void lock_finished(void* data, ext_session_lock_v1*) {
    static_cast<ClientState*>(data)->finished = true;
}
const ext_session_lock_v1_listener kLockListener{lock_locked, lock_finished};

void second_lock_finished(void* data, ext_session_lock_v1*) {
    ++static_cast<ClientState*>(data)->second_finished;
}
const ext_session_lock_v1_listener kSecondLockListener{[](void*, ext_session_lock_v1*) {},
                                                       second_lock_finished};

void lock_surface_configure(void* data, ext_session_lock_surface_v1* ls, uint32_t serial,
                            uint32_t width, uint32_t height) {
    auto* st = static_cast<ClientState*>(data);
    st->configure_serial = serial;
    st->configure_width = width;
    st->configure_height = height;
    ext_session_lock_surface_v1_ack_configure(ls, serial);
}
const ext_session_lock_surface_v1_listener kLockSurfaceListener{lock_surface_configure};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState& st = g_client;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display);
    if (st.compositor == nullptr || st.manager == nullptr || st.output == nullptr) {
        wl_display_disconnect(display);
        return;
    }

    st.lock = ext_session_lock_manager_v1_lock(st.manager);
    ext_session_lock_v1_add_listener(st.lock, &kLockListener, &st);
    wl_display_roundtrip(display); // the server grants it here

    // A second lock while one is held: refused, not queued.
    ext_session_lock_v1* second = ext_session_lock_manager_v1_lock(st.manager);
    ext_session_lock_v1_add_listener(second, &kSecondLockListener, &st);
    wl_display_roundtrip(display);
    ext_session_lock_v1_destroy(second); // never granted, so plain destroy is legal

    wl_surface* surface = wl_compositor_create_surface(st.compositor);
    ext_session_lock_surface_v1* ls =
        ext_session_lock_v1_get_lock_surface(st.lock, surface, st.output);
    ext_session_lock_surface_v1_add_listener(ls, &kLockSurfaceListener, &st);
    wl_display_roundtrip(display); // configure arrives and is acked
    wl_display_roundtrip(display);

    ext_session_lock_surface_v1_destroy(ls);
    ext_session_lock_v1_unlock_and_destroy(st.lock);
    wl_surface_destroy(surface);
    wl_display_roundtrip(display);
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
    auto output = luminaria::OutputGlobal::create(*display, 1920, 1080, "LOCK-1");
    assert(output.has_value());
    auto locks = luminaria::SessionLockManager::create(*display);
    assert(locks.has_value());

    assert(!locks->session_locked());
    assert(locks->current() == nullptr);

    int new_locks = 0;
    int new_surfaces = 0;
    bool locked_latch_on_grant = false;
    bool unlocked_orderly = false;
    bool locked_at_destroy = false;
    std::uint32_t serial = 0;
    luminaria::Signal<luminaria::NewSessionLockSurface>::Connection on_new_surface;
    luminaria::Signal<luminaria::SessionLockDestroy>::Connection on_lock_destroy;

    auto nl = locks->new_lock().connect([&](luminaria::NewSessionLock& e) {
        ++new_locks;
        luminaria::SessionLock& lock = e.lock;
        on_new_surface = lock.new_surface.connect([&](luminaria::NewSessionLockSurface& s) {
            ++new_surfaces;
            // One output, so it is sized to the whole of it.
            serial = s.lock_surface.configure(1920, 1080);
        });
        on_lock_destroy = lock.destroy.connect([&](luminaria::SessionLockDestroy& d) {
            unlocked_orderly = d.unlocked;
            locked_at_destroy = locks->session_locked();
        });
        // A real compositor blanks its outputs first; here there is nothing on
        // screen to hide, so the grant is immediate.
        lock.send_locked();
        locked_latch_on_grant = locks->session_locked();
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

    assert(new_locks == 1);              // the second lock never became ours
    assert(g_client.second_finished == 1);
    assert(g_client.locked);
    assert(!g_client.finished);
    assert(locked_latch_on_grant);
    assert(new_surfaces == 1);
    assert(serial != 0 && g_client.configure_serial == serial);
    assert(g_client.configure_width == 1920 && g_client.configure_height == 1080);
    // unlock_and_destroy: the latch drops BEFORE the destroy signal, so the
    // compositor sees a consistent "we are unlocked" while tearing down.
    assert(unlocked_orderly);
    assert(!locked_at_destroy);
    assert(!locks->session_locked());
    assert(locks->current() == nullptr);
    return 0;
}
