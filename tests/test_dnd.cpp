// Drag-and-drop over wl_data_device. The server gives the client pointer focus,
// the client starts a drag, and the server releases the button — which must
// produce data_device.enter (carrying an offer) followed by drop, after which
// the receiving side can still pull the payload over a pipe.
//
// Everything is one client here: what is being exercised is the compositor's
// drag state machine and its hand-off between wl_pointer and wl_data_device.
#include <cassert>
#include <cstddef>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

import luminaria;
import std;

namespace {

constexpr const char* kMime = "text/uri-list";
constexpr const char* kPayload = "file:///tmp/luminaria";

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
    bool got_drop = false;
    bool got_drop_performed = false;
    bool offer_has_mime = false;
    wl_data_offer* drag_offer = nullptr;
    std::string received;
};

ClientState g_client;

// --- source side ---
void source_target(void*, wl_data_source*, const char*) {}
void source_send(void*, wl_data_source*, const char* mime, int32_t fd) {
    if (mime != nullptr && std::strcmp(mime, kMime) == 0) {
        [[maybe_unused]] const ssize_t n = write(fd, kPayload, std::strlen(kPayload));
    }
    close(fd);
}
void source_cancelled(void*, wl_data_source*) {}
void source_dnd_drop_performed(void* data, wl_data_source*) {
    static_cast<ClientState*>(data)->got_drop_performed = true;
}
void source_dnd_finished(void*, wl_data_source*) {}
void source_action(void*, wl_data_source*, uint32_t) {}
const wl_data_source_listener kSourceListener{source_target,
                                              source_send,
                                              source_cancelled,
                                              source_dnd_drop_performed,
                                              source_dnd_finished,
                                              source_action};

// --- offer side ---
void offer_offer(void* data, wl_data_offer*, const char* mime) {
    if (mime != nullptr && std::strcmp(mime, kMime) == 0) {
        static_cast<ClientState*>(data)->offer_has_mime = true;
    }
}
void offer_source_actions(void*, wl_data_offer*, uint32_t) {}
void offer_action(void*, wl_data_offer*, uint32_t) {}
const wl_data_offer_listener kOfferListener{offer_offer, offer_source_actions, offer_action};

void device_data_offer(void* data, wl_data_device*, wl_data_offer* offer) {
    wl_data_offer_add_listener(offer, &kOfferListener, data);
}
void device_enter(void* data, wl_data_device*, uint32_t serial, wl_surface*, wl_fixed_t,
                  wl_fixed_t, wl_data_offer* offer) {
    auto* st = static_cast<ClientState*>(data);
    st->got_dnd_enter = true;
    st->drag_offer = offer;
    if (offer != nullptr) {
        wl_data_offer_accept(offer, serial, kMime);
        wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    }
}
void device_leave(void*, wl_data_device*) {}
void device_motion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void device_drop(void* data, wl_data_device*) {
    static_cast<ClientState*>(data)->got_drop = true;
}
void device_selection(void*, wl_data_device*, wl_data_offer*) {}
const wl_data_device_listener kDeviceListener{device_data_offer, device_enter,
                                              device_leave,      device_motion,
                                              device_drop,       device_selection};

// --- pointer: we only need the enter serial to legitimise start_drag ---
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
const wl_pointer_listener kPointerListener{ptr_enter,       ptr_leave,     ptr_motion,
                                           ptr_button,      ptr_axis,      ptr_frame,
                                           ptr_axis_source, ptr_axis_stop, ptr_axis_discrete};

void seat_caps(void*, wl_seat*, uint32_t) {}
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
    } else if (std::strcmp(interface, "wl_data_device_manager") == 0) {
        st->ddm = static_cast<wl_data_device_manager*>(
            wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3));
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

    if (st.compositor != nullptr && st.seat != nullptr && st.ddm != nullptr) {
        st.device = wl_data_device_manager_get_data_device(st.ddm, st.seat);
        wl_data_device_add_listener(st.device, &kDeviceListener, &st);

        st.surface = wl_compositor_create_surface(st.compositor);
        wl_surface_commit(st.surface); // commit #1: the server gives us pointer focus
        wl_display_roundtrip(display);
        wl_display_roundtrip(display); // pointer enter arrives

        wl_data_source* source = wl_data_device_manager_create_data_source(st.ddm);
        wl_data_source_add_listener(source, &kSourceListener, &st);
        wl_data_source_offer(source, kMime);
        wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        wl_data_device_start_drag(st.device, source, st.surface, nullptr, st.enter_serial);

        wl_surface_commit(st.surface); // commit #2: the server releases the button
        wl_display_roundtrip(display);
        wl_display_roundtrip(display); // dnd enter + drop arrive

        if (st.drag_offer != nullptr && st.got_drop) {
            int pipe_fds[2];
            if (pipe(pipe_fds) == 0) {
                wl_data_offer_receive(st.drag_offer, kMime, pipe_fds[1]);
                close(pipe_fds[1]);
                wl_display_roundtrip(display);
                char buf[128] = {};
                const ssize_t n = read(pipe_fds[0], buf, sizeof(buf) - 1);
                close(pipe_fds[0]);
                if (n > 0) {
                    st.received.assign(buf, static_cast<size_t>(n));
                }
            }
            wl_data_offer_finish(st.drag_offer);
            wl_display_roundtrip(display);
        }
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
    auto data_device = luminaria::DataDeviceManager::create(*display, *seat);
    assert(data_device.has_value());

    bool dragging_seen = false;
    int commits = 0;
    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* surface = &e.surface;
        conns.push_back(e.surface.commit.connect([&, surface](luminaria::SurfaceCommit&) {
            ++commits;
            if (commits == 1) {
                seat->set_keyboard_focus(surface);
                seat->pointer_enter(*surface, 5, 5);
            } else if (commits == 2) {
                // The drag is running by now; releasing the button drops.
                dragging_seen = seat->dragging() && data_device->dragging();
                seat->pointer_button(272 /*BTN_LEFT*/, false);
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

    assert(g_client.got_pointer_enter);
    assert(dragging_seen);          // the seat handed the pointer to the drag
    assert(g_client.got_dnd_enter); // wl_data_device.enter, with an offer
    assert(g_client.offer_has_mime);
    assert(g_client.got_drop);
    assert(g_client.got_drop_performed);
    assert(g_client.received == kPayload);
    assert(!seat->dragging()); // the drag ended cleanly
    return 0;
}
