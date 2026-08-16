// A client may destroy the surface a drag is over, mid-drag.
//
// DataDeviceManager caches that surface as a raw `Surface*` (drag_focus) and
// dereferences it on every motion and on the drop, to find which client to
// send to. Seat's own destroy handler only clears Seat's copy of the pointer
// focus — it does not tell the drag — so without a destroy subscription of its
// own the data-device keeps a pointer to freed memory and the drop reads it.
//
// The sequence here is the reachable one: pointer focus, start_drag (which
// latches drag_focus immediately), destroy the surface, then release the
// button. Before the fix this run used freed memory in drag_drop().
#include <cassert>
#include <cstddef>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-server-core.h>

import luminaria;
import std;

namespace {

constexpr const char* kMime = "text/plain";

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_data_device_manager* ddm = nullptr;
    wl_data_device* device = nullptr;
    wl_surface* surface = nullptr;

    uint32_t enter_serial = 0;
    bool got_pointer_enter = false;
    bool got_dnd_enter = false;
    bool got_leave = false;
    // Set by whichever way the drag ends. With the drop target destroyed the
    // compositor cancels the source; with it alive it reports the drop.
    bool drag_finished = false;
};
ClientState g_client;

void source_target(void*, wl_data_source*, const char*) {}
void source_send(void*, wl_data_source*, const char*, int32_t fd) { close(fd); }
void source_cancelled(void* data, wl_data_source*) {
    static_cast<ClientState*>(data)->drag_finished = true;
}
void source_dnd_drop_performed(void* data, wl_data_source*) {
    static_cast<ClientState*>(data)->drag_finished = true;
}
void source_dnd_finished(void*, wl_data_source*) {}
void source_action(void*, wl_data_source*, uint32_t) {}
const wl_data_source_listener kSourceListener{source_target,
                                              source_send,
                                              source_cancelled,
                                              source_dnd_drop_performed,
                                              source_dnd_finished,
                                              source_action};

void offer_offer(void*, wl_data_offer*, const char*) {}
void offer_source_actions(void*, wl_data_offer*, uint32_t) {}
void offer_action(void*, wl_data_offer*, uint32_t) {}
const wl_data_offer_listener kOfferListener{offer_offer, offer_source_actions, offer_action};

void device_data_offer(void*, wl_data_device*, wl_data_offer* offer) {
    wl_data_offer_add_listener(offer, &kOfferListener, nullptr);
}
void device_enter(void* data, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t,
                  wl_data_offer*) {
    static_cast<ClientState*>(data)->got_dnd_enter = true;
}
void device_leave(void* data, wl_data_device*) {
    static_cast<ClientState*>(data)->got_leave = true;
}
void device_motion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void device_drop(void*, wl_data_device*) {}
void device_selection(void*, wl_data_device*, wl_data_offer*) {}
const wl_data_device_listener kDeviceListener{device_data_offer, device_enter,  device_leave,
                                              device_motion,     device_drop,   device_selection};

void ptr_enter(void* data, wl_pointer*, uint32_t serial, wl_surface*, wl_fixed_t, wl_fixed_t) {
    auto* st = static_cast<ClientState*>(data);
    st->enter_serial = serial;
    st->got_pointer_enter = true;
}
void ptr_leave(void*, wl_pointer*, uint32_t, wl_surface*) {}
void ptr_motion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void ptr_button(void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) {}
void ptr_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
void ptr_frame(void*, wl_pointer*) {}
void ptr_axis_source(void*, wl_pointer*, uint32_t) {}
void ptr_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
void ptr_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
const wl_pointer_listener kPointerListener{
    ptr_enter,       ptr_leave,       ptr_motion,      ptr_button,       ptr_axis,
    ptr_frame,       ptr_axis_source, ptr_axis_stop,   ptr_axis_discrete};

void seat_caps(void* data, wl_seat* seat, uint32_t caps) {
    auto* st = static_cast<ClientState*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) != 0 && st->pointer == nullptr) {
        st->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(st->pointer, &kPointerListener, st);
    }
}
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener kSeatListener{seat_caps, seat_name};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "wl_compositor") == 0) {
        st->compositor =
            static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 1));
    } else if (std::strcmp(iface, "wl_seat") == 0) {
        st->seat = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, 1));
        wl_seat_add_listener(st->seat, &kSeatListener, st);
    } else if (std::strcmp(iface, "wl_data_device_manager") == 0) {
        st->ddm = static_cast<wl_data_device_manager*>(
            wl_registry_bind(reg, name, &wl_data_device_manager_interface, 1));
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
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistryListener, &st);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); // seat capabilities → wl_pointer

    if (st.compositor != nullptr && st.seat != nullptr && st.ddm != nullptr) {
        st.device = wl_data_device_manager_get_data_device(st.ddm, st.seat);
        wl_data_device_add_listener(st.device, &kDeviceListener, &st);

        st.surface = wl_compositor_create_surface(st.compositor);
        wl_surface_commit(st.surface); // the server gives us pointer focus here
        wl_display_roundtrip(display);
        wl_display_roundtrip(display);

        wl_data_source* source = wl_data_device_manager_create_data_source(st.ddm);
        wl_data_source_add_listener(source, &kSourceListener, &st);
        wl_data_source_offer(source, kMime);
        wl_data_device_start_drag(st.device, source, st.surface, nullptr, st.enter_serial);
        wl_display_roundtrip(display);

        // The drag is live and the compositor is holding this surface as its
        // drop target. Pull it out from under it.
        wl_surface_destroy(st.surface);
        st.surface = nullptr;
        wl_display_roundtrip(display);

        // Block until the compositor has actually run the drop against the
        // destroyed target. Disconnecting earlier would end the server's loop
        // before the interesting code ran. The iteration cap matters: a
        // regression here corrupts the drag state rather than raising, and an
        // uncapped loop would hang the whole suite instead of failing it.
        for (int i = 0; i < 64 && !st.drag_finished; ++i) {
            if (wl_display_dispatch(display) < 0) {
                break;
            }
        }
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
    auto seat = luminaria::Seat::create(*display);
    assert(seat.has_value());
    auto data_device = luminaria::DataDeviceManager::create(*display, *seat);
    assert(data_device.has_value());

    bool drag_started = false;
    bool surface_destroyed = false;
    bool dropped = false;

    // Releasing the button from inside the destroy emit would run while the
    // Surface is still mid-destruction; a short timer puts the drop just after
    // it, which is exactly when a real compositor's next input event would
    // land. (1ms, not 0: libwayland reads 0 as "disarm".)
    auto drop_timer = display->event_loop().add_timer([&] {
        seat->pointer_button(272 /*BTN_LEFT*/, false); // dereferences drag_focus
        dropped = true;
    });

    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> commit_conns;
    std::vector<luminaria::Signal<luminaria::SurfaceDestroy>::Connection> destroy_conns;

    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* surface = &e.surface;
        commit_conns.push_back(e.surface.commit.connect([&, surface](luminaria::SurfaceCommit&) {
            seat->set_keyboard_focus(surface);
            seat->pointer_enter(*surface, 5, 5);
        }));
        destroy_conns.push_back(e.surface.destroy.connect([&](luminaria::SurfaceDestroy&) {
            // Sampled here, which is the moment that matters: the drag must be
            // live and holding this very surface as its target.
            drag_started = data_device->dragging();
            surface_destroyed = true;
            drop_timer.arm(1);
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

    assert(g_client.got_pointer_enter);
    assert(drag_started);      // drag_focus was latched to the surface
    assert(surface_destroyed); // ...and then the surface went away
    assert(dropped);           // the drop ran against a destroyed drop target
    assert(!seat->dragging()); // and ended the drag cleanly instead of faulting
    return 0;
}
