// The rest of wl_seat: pointer scrolling (smooth, notched, and the stop event),
// touch down/motion/up/frame, and wl_pointer.set_cursor. The client asserts the
// wire events it received; the server asserts it saw the cursor the client set.
#include <cassert>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

import luminaria;

namespace {

constexpr int kHotspotX = 3;
constexpr int kHotspotY = 4;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_touch* touch = nullptr;
    wl_surface* cursor_surface = nullptr;
    uint32_t capabilities = 0;

    bool got_axis = false;
    bool got_axis_source = false;
    bool got_axis_discrete = false;
    bool got_axis_stop = false;
    double axis_value = 0.0;
    int32_t axis_discrete = 0;

    bool got_touch_down = false;
    bool got_touch_motion = false;
    bool got_touch_up = false;
    bool got_touch_frame = false;
    double touch_x = 0.0, touch_y = 0.0;
};

ClientState g_client;

void ptr_enter(void* data, wl_pointer* pointer, uint32_t serial, wl_surface*, wl_fixed_t,
               wl_fixed_t) {
    auto* st = static_cast<ClientState*>(data);
    // Now that we hold pointer focus we're allowed to set the cursor image.
    wl_pointer_set_cursor(pointer, serial, st->cursor_surface, kHotspotX, kHotspotY);
}
void ptr_leave(void*, wl_pointer*, uint32_t, wl_surface*) {}
void ptr_motion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void ptr_button(void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) {}
void ptr_axis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    auto* st = static_cast<ClientState*>(data);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        st->got_axis = true;
        st->axis_value = wl_fixed_to_double(value);
    }
}
void ptr_frame(void*, wl_pointer*) {}
void ptr_axis_source(void* data, wl_pointer*, uint32_t) {
    static_cast<ClientState*>(data)->got_axis_source = true;
}
void ptr_axis_stop(void* data, wl_pointer*, uint32_t, uint32_t axis) {
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        static_cast<ClientState*>(data)->got_axis_stop = true;
    }
}
void ptr_axis_discrete(void* data, wl_pointer*, uint32_t axis, int32_t discrete) {
    auto* st = static_cast<ClientState*>(data);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        st->got_axis_discrete = true;
        st->axis_discrete = discrete;
    }
}
const wl_pointer_listener kPointerListener{ptr_enter,       ptr_leave,     ptr_motion,
                                           ptr_button,      ptr_axis,      ptr_frame,
                                           ptr_axis_source, ptr_axis_stop, ptr_axis_discrete};

void touch_down(void* data, wl_touch*, uint32_t, uint32_t, wl_surface*, int32_t, wl_fixed_t x,
                wl_fixed_t y) {
    auto* st = static_cast<ClientState*>(data);
    st->got_touch_down = true;
    st->touch_x = wl_fixed_to_double(x);
    st->touch_y = wl_fixed_to_double(y);
}
void touch_up(void* data, wl_touch*, uint32_t, uint32_t, int32_t) {
    static_cast<ClientState*>(data)->got_touch_up = true;
}
void touch_motion(void* data, wl_touch*, uint32_t, int32_t, wl_fixed_t, wl_fixed_t) {
    static_cast<ClientState*>(data)->got_touch_motion = true;
}
void touch_frame(void* data, wl_touch*) {
    static_cast<ClientState*>(data)->got_touch_frame = true;
}
void touch_cancel(void*, wl_touch*) {}
const wl_touch_listener kTouchListener{touch_down, touch_up, touch_motion, touch_frame,
                                       touch_cancel};

void seat_caps(void* data, wl_seat*, uint32_t caps) {
    static_cast<ClientState*>(data)->capabilities = caps;
}
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener kSeatListener{seat_caps, seat_name};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_seat") == 0) {
        st->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 5));
        wl_seat_add_listener(st->seat, &kSeatListener, st);
        st->pointer = wl_seat_get_pointer(st->seat);
        wl_pointer_add_listener(st->pointer, &kPointerListener, st);
        st->touch = wl_seat_get_touch(st->seat);
        wl_touch_add_listener(st->touch, &kTouchListener, st);
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

    if (st.compositor != nullptr && st.seat != nullptr) {
        st.cursor_surface = wl_compositor_create_surface(st.compositor);
        wl_surface* surface = wl_compositor_create_surface(st.compositor);
        wl_surface_commit(surface);    // the server injects all the input here
        wl_display_roundtrip(display); // flush; the server processes the commit
        wl_display_roundtrip(display); // receive the input events, reply set_cursor
        wl_display_roundtrip(display); // the server processes set_cursor
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
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto seat = luminaria::Seat::create(*display);
    assert(seat.has_value());
    seat->set_capabilities(true, true, true); // this compositor has a touchscreen

    bool cursor_set = false;
    int cursor_hotspot_x = -1, cursor_hotspot_y = -1;
    bool cursor_surface_is_first = false;
    luminaria::Surface* first_surface = nullptr;
    int surfaces = 0;

    auto cc = seat->cursor_changed().connect([&](luminaria::SeatCursorChange& e) {
        luminaria::Surface* cursor = luminaria::surface_from_id(e.surface);
        if (cursor == nullptr) {
            return; // the "cursor is gone" notification at teardown
        }
        cursor_set = true;
        cursor_hotspot_x = e.hotspot_x;
        cursor_hotspot_y = e.hotspot_y;
        cursor_surface_is_first = cursor == first_surface;
    });

    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* surface = &e.surface;
        if (++surfaces == 1) {
            first_surface = surface; // the client makes the cursor surface first
        }
        conns.push_back(e.surface.commit.connect([&, surface](luminaria::SurfaceCommit&) {
            seat->set_keyboard_focus(surface);
            seat->pointer_enter(*surface, 1.0, 2.0);
            seat->pointer_axis_discrete(0, 1);       // one wheel notch down
            seat->pointer_axis(0.0, 12.0);           // smooth scroll
            seat->pointer_axis_stop(false, true);    // fingers lifted
            seat->touch_down(*surface, 0, 11.0, 22.0);
            seat->touch_motion(0, 12.0, 23.0);
            seat->touch_up(0);
            seat->touch_frame();
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

    assert((g_client.capabilities & WL_SEAT_CAPABILITY_TOUCH) != 0);
    assert(g_client.got_axis_source);
    assert(g_client.got_axis_discrete && g_client.axis_discrete == 1);
    assert(g_client.got_axis && g_client.axis_value == 12.0);
    assert(g_client.got_axis_stop);

    assert(g_client.got_touch_down);
    assert(g_client.touch_x == 11.0 && g_client.touch_y == 22.0);
    assert(g_client.got_touch_motion);
    assert(g_client.got_touch_up);
    assert(g_client.got_touch_frame);

    assert(cursor_set);
    assert(cursor_surface_is_first);
    assert(cursor_hotspot_x == kHotspotX && cursor_hotspot_y == kHotspotY);
    // Client teardown invalidates every retained identity in the seat. None of
    // these may resolve to a future surface that reuses the registry slot.
    assert(!seat->keyboard_focus().valid());
    assert(!seat->pointer_focus().valid());
    assert(!seat->cursor_surface().valid());
    return 0;
}
