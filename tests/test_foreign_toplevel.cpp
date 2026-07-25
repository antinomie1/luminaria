// zwlr_foreign_toplevel_management_v1: a taskbar that starts AFTER a window is
// already mapped still gets the whole list, sees its title and app id, and can
// ask for it to be activated and closed. Nothing here is published by hand —
// ForeignToplevelManager::track() mirrors the xdg shell on its own.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

import luminaria;

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_surface* surface = nullptr;
    bool buffered = false;
    bool got_close = false;

    // The taskbar half.
    uint32_t manager_name = 0;
    zwlr_foreign_toplevel_manager_v1* manager = nullptr;
    zwlr_foreign_toplevel_handle_v1* handle = nullptr;
    std::string seen_title;
    std::string seen_app_id;
    bool seen_activated = false;
    int dones = 0;
};

ClientState g_client;

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

// ---- the ordinary application half ----

void xdg_surface_configure(void* data, xdg_surface* xsurf, uint32_t serial) {
    auto* st = static_cast<ClientState*>(data);
    xdg_surface_ack_configure(xsurf, serial);
    if (!st->buffered) {
        wl_surface_attach(st->surface, make_buffer(st, 64, 64), 0, 0);
        wl_surface_commit(st->surface);
        st->buffered = true;
    }
}
const xdg_surface_listener kXdgSurfaceListener{xdg_surface_configure};

void toplevel_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
void toplevel_close(void* data, xdg_toplevel*) {
    static_cast<ClientState*>(data)->got_close = true;
}
void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener kToplevelListener{toplevel_configure, toplevel_close,
                                              toplevel_configure_bounds,
                                              toplevel_wm_capabilities};

void wm_base_ping(void*, xdg_wm_base* wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}
const xdg_wm_base_listener kWmBaseListener{wm_base_ping};

// ---- the taskbar half ----

void ftl_title(void* data, zwlr_foreign_toplevel_handle_v1*, const char* title) {
    static_cast<ClientState*>(data)->seen_title = title;
}
void ftl_app_id(void* data, zwlr_foreign_toplevel_handle_v1*, const char* app_id) {
    static_cast<ClientState*>(data)->seen_app_id = app_id;
}
void ftl_output_enter(void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) {}
void ftl_output_leave(void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) {}
void ftl_state(void* data, zwlr_foreign_toplevel_handle_v1*, wl_array* states) {
    auto* st = static_cast<ClientState*>(data);
    st->seen_activated = false;
    const auto* first = static_cast<const uint32_t*>(states->data);
    for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i) {
        if (first[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
            st->seen_activated = true;
        }
    }
}
void ftl_done(void* data, zwlr_foreign_toplevel_handle_v1*) {
    ++static_cast<ClientState*>(data)->dones;
}
void ftl_closed(void*, zwlr_foreign_toplevel_handle_v1*) {}
void ftl_parent(void*, zwlr_foreign_toplevel_handle_v1*, zwlr_foreign_toplevel_handle_v1*) {}
const zwlr_foreign_toplevel_handle_v1_listener kHandleListener{
    ftl_title, ftl_app_id, ftl_output_enter, ftl_output_leave,
    ftl_state, ftl_done,   ftl_closed,       ftl_parent};

void manager_toplevel(void* data, zwlr_foreign_toplevel_manager_v1*,
                      zwlr_foreign_toplevel_handle_v1* handle) {
    auto* st = static_cast<ClientState*>(data);
    st->handle = handle;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &kHandleListener, st);
}
void manager_finished(void*, zwlr_foreign_toplevel_manager_v1*) {}
const zwlr_foreign_toplevel_manager_v1_listener kManagerListener{manager_toplevel,
                                                                 manager_finished};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_shm") == 0) {
        st->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, "wl_seat") == 0) {
        st->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 5));
    } else if (std::strcmp(interface, "xdg_wm_base") == 0) {
        st->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 5));
    } else if (std::strcmp(interface, "zwlr_foreign_toplevel_manager_v1") == 0) {
        // Deliberately not bound yet: the taskbar arrives late, after the
        // window is already on screen.
        st->manager_name = name;
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState& st = g_client;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display);

    if (st.compositor == nullptr || st.shm == nullptr || st.wm_base == nullptr ||
        st.seat == nullptr || st.manager_name == 0) {
        wl_display_disconnect(display);
        return;
    }

    xdg_wm_base_add_listener(st.wm_base, &kWmBaseListener, &st);
    st.surface = wl_compositor_create_surface(st.compositor);
    xdg_surface* xsurf = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
    xdg_surface_add_listener(xsurf, &kXdgSurfaceListener, &st);
    xdg_toplevel* toplevel = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(toplevel, &kToplevelListener, &st);
    xdg_toplevel_set_title(toplevel, "Bar");
    xdg_toplevel_set_app_id(toplevel, "org.luminaria.bar");
    wl_surface_commit(st.surface); // initial commit -> configure
    wl_display_roundtrip(display); // ack + buffer
    wl_display_roundtrip(display); // the server sees the mapping commit

    // Now the taskbar starts up and should be handed the existing window.
    st.manager = static_cast<zwlr_foreign_toplevel_manager_v1*>(
        wl_registry_bind(registry, st.manager_name,
                         &zwlr_foreign_toplevel_manager_v1_interface, 3));
    zwlr_foreign_toplevel_manager_v1_add_listener(st.manager, &kManagerListener, &st);
    wl_display_roundtrip(display); // toplevel + its details
    wl_display_roundtrip(display);
    assert(st.handle != nullptr);
    assert(!st.seen_activated);

    zwlr_foreign_toplevel_handle_v1_activate(st.handle, st.seat);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); // the state update comes back

    zwlr_foreign_toplevel_handle_v1_close(st.handle);
    wl_display_roundtrip(display);
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
    assert(display->init_shm().has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto seat = luminaria::Seat::create(*display);
    assert(seat.has_value());
    auto shell = luminaria::XdgShell::create(*display);
    assert(shell.has_value());
    auto windows = luminaria::ForeignToplevelManager::create(*display);
    assert(windows.has_value());
    windows->track(*shell);

    bool saw_activate = false;
    bool saw_close = false;
    auto conn = windows->request().connect([&](luminaria::ForeignToplevelRequest& r) {
        switch (r.kind) {
        case luminaria::ForeignToplevelRequest::Kind::activate:
            saw_activate = r.seat != nullptr;
            r.toplevel.set_activated(true);
            break;
        case luminaria::ForeignToplevelRequest::Kind::close:
            saw_close = true;
            r.toplevel.close();
            break;
        default:
            break;
        }
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

    assert(g_client.handle != nullptr);
    assert(g_client.seen_title == "Bar");
    assert(g_client.seen_app_id == "org.luminaria.bar");
    assert(g_client.dones >= 2); // the initial dump, then the state update
    assert(g_client.seen_activated);
    assert(g_client.got_close);
    assert(saw_activate);
    assert(saw_close);
    return 0;
}
