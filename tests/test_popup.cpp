// xdg_popup + xdg_positioner: a client maps a toplevel, then opens a popup
// anchored to it. The client asserts the geometry the server computed from the
// positioner arrives in xdg_popup.configure; the server asserts new_popup/map
// fired with the same numbers, and that reposition moves it.
#include <cassert>
#include <cstddef>
#include <cstring>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "xdg-shell-client-protocol.h"

import luminaria;
import std;

namespace {

// anchor rect (10,10 20x20), anchor BOTTOM_LEFT -> anchor point (10, 30);
// gravity BOTTOM_RIGHT -> the popup grows right/down from there; offset (5,5).
constexpr int kExpectX = 15;
constexpr int kExpectY = 35;
constexpr int kPopupW = 100;
constexpr int kPopupH = 50;
// The constrained popup: 40x40, anchored to a rect at y=180 on a 200x200
// output, so unflipped it would run 20px off the bottom.
constexpr int kEdgeW = 40;
constexpr int kEdgeH = 40;
constexpr int kOutputSize = 200;
constexpr int kFlippedY = 140; // 180 (anchor top) - 40 (grown upwards)
// After reposition the offset becomes (0,0), so the popup lands on the anchor.
constexpr int kRepositionedX = 10;
constexpr int kRepositionedY = 30;
constexpr uint32_t kRepositionToken = 4242;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;

    wl_surface* toplevel_surface = nullptr;
    wl_surface* popup_surface = nullptr;
    xdg_surface* popup_xdg = nullptr;
    bool toplevel_buffered = false;
    bool popup_buffered = false;

    int cfg_x = -1, cfg_y = -1, cfg_w = -1, cfg_h = -1;
    int configures = 0;
    bool got_repositioned = false;
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

void toplevel_xdg_configure(void* data, xdg_surface* xsurf, uint32_t serial) {
    auto* st = static_cast<ClientState*>(data);
    xdg_surface_ack_configure(xsurf, serial);
    if (!st->toplevel_buffered) {
        wl_surface_attach(st->toplevel_surface, make_buffer(st, 64, 64), 0, 0);
        wl_surface_commit(st->toplevel_surface);
        st->toplevel_buffered = true;
    }
}
const xdg_surface_listener kToplevelXdgListener{toplevel_xdg_configure};

void popup_xdg_configure(void* data, xdg_surface* xsurf, uint32_t serial) {
    auto* st = static_cast<ClientState*>(data);
    xdg_surface_ack_configure(xsurf, serial);
    if (!st->popup_buffered) {
        wl_surface_attach(st->popup_surface, make_buffer(st, kPopupW, kPopupH), 0, 0);
        wl_surface_commit(st->popup_surface);
        st->popup_buffered = true;
    }
}
const xdg_surface_listener kPopupXdgListener{popup_xdg_configure};

void toplevel_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
void toplevel_close(void*, xdg_toplevel*) {}
void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener kToplevelListener{toplevel_configure, toplevel_close,
                                              toplevel_configure_bounds,
                                              toplevel_wm_capabilities};

void popup_configure(void* data, xdg_popup*, int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* st = static_cast<ClientState*>(data);
    st->cfg_x = x;
    st->cfg_y = y;
    st->cfg_w = w;
    st->cfg_h = h;
    ++st->configures;
}
void popup_done(void*, xdg_popup*) {}
void popup_repositioned(void* data, xdg_popup*, uint32_t token) {
    auto* st = static_cast<ClientState*>(data);
    st->got_repositioned = token == kRepositionToken;
}
const xdg_popup_listener kPopupListener{popup_configure, popup_done, popup_repositioned};

void wm_base_ping(void*, xdg_wm_base* wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}
const xdg_wm_base_listener kWmBaseListener{wm_base_ping};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_shm") == 0) {
        st->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, "xdg_wm_base") == 0) {
        st->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 5));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

ClientState g_client;

xdg_positioner* make_positioner(ClientState* st, int32_t offset_x, int32_t offset_y) {
    xdg_positioner* pos = xdg_wm_base_create_positioner(st->wm_base);
    xdg_positioner_set_size(pos, kPopupW, kPopupH);
    xdg_positioner_set_anchor_rect(pos, 10, 10, 20, 20);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_offset(pos, offset_x, offset_y);
    return pos;
}

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState& st = g_client;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display);

    if (st.compositor != nullptr && st.shm != nullptr && st.wm_base != nullptr) {
        xdg_wm_base_add_listener(st.wm_base, &kWmBaseListener, &st);

        st.toplevel_surface = wl_compositor_create_surface(st.compositor);
        xdg_surface* parent_xdg = xdg_wm_base_get_xdg_surface(st.wm_base, st.toplevel_surface);
        xdg_surface_add_listener(parent_xdg, &kToplevelXdgListener, &st);
        xdg_toplevel* toplevel = xdg_surface_get_toplevel(parent_xdg);
        xdg_toplevel_add_listener(toplevel, &kToplevelListener, &st);
        wl_surface_commit(st.toplevel_surface);
        wl_display_roundtrip(display); // configure -> ack -> buffer -> map
        wl_display_roundtrip(display);

        st.popup_surface = wl_compositor_create_surface(st.compositor);
        st.popup_xdg = xdg_wm_base_get_xdg_surface(st.wm_base, st.popup_surface);
        xdg_surface_add_listener(st.popup_xdg, &kPopupXdgListener, &st);
        xdg_positioner* pos = make_positioner(&st, 5, 5);
        xdg_popup* popup = xdg_surface_get_popup(st.popup_xdg, parent_xdg, pos);
        xdg_positioner_destroy(pos);
        xdg_popup_add_listener(popup, &kPopupListener, &st);
        wl_surface_commit(st.popup_surface); // initial commit -> popup configure
        wl_display_roundtrip(display);
        wl_display_roundtrip(display); // the mapping commit lands

        // Move it: same anchor, no offset.
        xdg_positioner* pos2 = make_positioner(&st, 0, 0);
        xdg_popup_reposition(popup, pos2, kRepositionToken);
        xdg_positioner_destroy(pos2);
        wl_display_roundtrip(display);

        // A second popup anchored near the bottom of the output, asking to be
        // flipped rather than left hanging off the edge.
        wl_surface* edge_surface = wl_compositor_create_surface(st.compositor);
        xdg_surface* edge_xdg = xdg_wm_base_get_xdg_surface(st.wm_base, edge_surface);
        xdg_surface_add_listener(edge_xdg, &kPopupXdgListener, &st);
        xdg_positioner* edge_pos = xdg_wm_base_create_positioner(st.wm_base);
        xdg_positioner_set_size(edge_pos, kEdgeW, kEdgeH);
        xdg_positioner_set_anchor_rect(edge_pos, 10, 180, 20, 20);
        xdg_positioner_set_anchor(edge_pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
        xdg_positioner_set_gravity(edge_pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(
            edge_pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
        xdg_popup* edge_popup = xdg_surface_get_popup(edge_xdg, parent_xdg, edge_pos);
        xdg_positioner_destroy(edge_pos);
        wl_surface_commit(edge_surface);
        wl_display_roundtrip(display);
        (void)edge_popup;
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
    auto shell = luminaria::XdgShell::create(*display);
    assert(shell.has_value());

    bool saw_popup = false;
    bool saw_popup_map = false;
    bool popup_parent_is_toplevel_surface = false;
    int popup_x = -1, popup_y = -1, popup_w = -1, popup_h = -1;
    int repositioned_x = -1, repositioned_y = -1;
    luminaria::Surface* toplevel_surface = nullptr;
    std::vector<luminaria::Signal<luminaria::ToplevelMap>::Connection> tl_conns;
    std::vector<luminaria::Signal<luminaria::PopupMap>::Connection> popup_conns;
    std::vector<luminaria::Signal<luminaria::PopupReposition>::Connection> repos_conns;

    auto nt = shell->new_toplevel().connect([&](luminaria::NewToplevel& e) {
        toplevel_surface = &e.toplevel.surface();
        tl_conns.push_back(e.toplevel.map.connect([](luminaria::ToplevelMap&) {}));
    });
    // Without this the shell has no idea where the parent window is and cannot
    // apply constraint_adjustment at all. Here the window sits at the origin of
    // a 200x200 output.
    shell->set_popup_constraint_query(
        [&](luminaria::Surface& parent, luminaria::Box& parent_box, luminaria::Box& usable) {
            if (&parent != toplevel_surface) {
                return false;
            }
            parent_box = luminaria::Box{0, 0, kOutputSize, kOutputSize};
            usable = luminaria::Box{0, 0, kOutputSize, kOutputSize};
            return true;
        });

    int popups_seen = 0;
    int flipped_x = -1, flipped_y = -1;
    auto np = shell->new_popup().connect([&](luminaria::NewPopup& e) {
        if (++popups_seen == 2) {
            // The second popup is the one that had to be flipped.
            flipped_x = e.popup.x();
            flipped_y = e.popup.y();
            return;
        }
        saw_popup = true;
        luminaria::Popup* popup = &e.popup;
        popup_x = popup->x();
        popup_y = popup->y();
        popup_w = popup->width();
        popup_h = popup->height();
        popup_parent_is_toplevel_surface = popup->parent_surface() == toplevel_surface;
        popup_conns.push_back(
            e.popup.map.connect([&](luminaria::PopupMap&) { saw_popup_map = true; }));
        repos_conns.push_back(
            e.popup.reposition.connect([&, popup](luminaria::PopupReposition&) {
                repositioned_x = popup->x();
                repositioned_y = popup->y();
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

    assert(saw_popup);
    assert(saw_popup_map);
    assert(popup_parent_is_toplevel_surface);
    assert(popup_x == kExpectX && popup_y == kExpectY);
    assert(popup_w == kPopupW && popup_h == kPopupH);
    assert(repositioned_x == kRepositionedX && repositioned_y == kRepositionedY);

    // FLIP_Y: anchored to the bottom of a rect at y=180 and growing downwards,
    // the popup would end at y=240 on a 200px output. Flipping anchors it to
    // the TOP of the same rect and grows upwards instead, which fits.
    assert(popups_seen == 2);
    assert(flipped_x == 10);
    assert(flipped_y == kFlippedY);

    // The client saw exactly the same rectangle on the wire.
    assert(g_client.configures >= 2);
    assert(g_client.cfg_x == kRepositionedX && g_client.cfg_y == kRepositionedY);
    assert(g_client.cfg_w == kPopupW && g_client.cfg_h == kPopupH);
    assert(g_client.got_repositioned);
    return 0;
}
