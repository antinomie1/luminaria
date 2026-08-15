// zwlr_data_control_manager_v1, both directions across the bridge:
//
//   A. an ordinary client sets the clipboard with wl_data_source → the
//      data-control device is told, and receiving through its offer pulls the
//      bytes out of the ordinary source.
//   B. the data-control client sets the clipboard with its own source → the
//      focused ordinary client sees a normal wl_data_offer, and receiving
//      through THAT pulls the bytes out of the data-control source. This is
//      the direction that goes through `SelectionSource`, so it is the one
//      that proves the ordinary clipboard no longer needs a wl_data_source.
//
// One process plays both roles, which is exactly the point: data-control works
// without focus, and here it is even the same client.
#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "wlr-data-control-unstable-v1-client-protocol.h"

import luminaria.desktop;

namespace {

constexpr const char* kMimeA = "text/plain";
constexpr const char* kMimeB = "text/html";
constexpr const char* kPayloadA = "from-wl-data-source";
constexpr const char* kPayloadB = "from-data-control";

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    wl_data_device_manager* ddm = nullptr;
    zwlr_data_control_manager_v1* control = nullptr;

    wl_data_device* device = nullptr;
    zwlr_data_control_device_v1* control_device = nullptr;

    // Direction A
    zwlr_data_control_offer_v1* control_offer = nullptr;
    std::vector<std::string> control_offer_mimes;
    int control_selections = 0;

    // Direction B
    wl_data_offer* data_offer = nullptr;
    std::vector<std::string> data_offer_mimes;
    int data_selections = 0;

    bool control_source_cancelled = false;
};

ClientState g_client;

// --- direction A: our wl_data_source is asked for the bytes ---
void source_target(void*, wl_data_source*, const char*) {}
void source_send(void*, wl_data_source*, const char*, int32_t fd) {
    const auto n = write(fd, kPayloadA, std::strlen(kPayloadA));
    static_cast<void>(n);
    close(fd);
}
void source_cancelled(void*, wl_data_source*) {}
void source_dnd_drop_performed(void*, wl_data_source*) {}
void source_dnd_finished(void*, wl_data_source*) {}
void source_action(void*, wl_data_source*, uint32_t) {}
const wl_data_source_listener kSourceListener{source_target,
                                              source_send,
                                              source_cancelled,
                                              source_dnd_drop_performed,
                                              source_dnd_finished,
                                              source_action};

// --- direction B: our data-control source is asked for the bytes ---
void control_source_send(void*, zwlr_data_control_source_v1*, const char*, int32_t fd) {
    const auto n = write(fd, kPayloadB, std::strlen(kPayloadB));
    static_cast<void>(n);
    close(fd);
}
void control_source_cancelled(void* data, zwlr_data_control_source_v1*) {
    static_cast<ClientState*>(data)->control_source_cancelled = true;
}
const zwlr_data_control_source_v1_listener kControlSourceListener{control_source_send,
                                                                  control_source_cancelled};

// --- offers ---
void control_offer_offer(void* data, zwlr_data_control_offer_v1*, const char* mime) {
    static_cast<ClientState*>(data)->control_offer_mimes.emplace_back(mime);
}
const zwlr_data_control_offer_v1_listener kControlOfferListener{control_offer_offer};

void control_device_data_offer(void* data, zwlr_data_control_device_v1*,
                               zwlr_data_control_offer_v1* offer) {
    auto* st = static_cast<ClientState*>(data);
    st->control_offer_mimes.clear();
    zwlr_data_control_offer_v1_add_listener(offer, &kControlOfferListener, st);
}
void control_device_selection(void* data, zwlr_data_control_device_v1*,
                              zwlr_data_control_offer_v1* offer) {
    auto* st = static_cast<ClientState*>(data);
    ++st->control_selections;
    st->control_offer = offer;
}
void control_device_finished(void*, zwlr_data_control_device_v1*) {}
void control_device_primary_selection(void*, zwlr_data_control_device_v1*,
                                      zwlr_data_control_offer_v1*) {}
const zwlr_data_control_device_v1_listener kControlDeviceListener{
    control_device_data_offer, control_device_selection, control_device_finished,
    control_device_primary_selection};

void data_offer_offer(void* data, wl_data_offer*, const char* mime) {
    static_cast<ClientState*>(data)->data_offer_mimes.emplace_back(mime);
}
void data_offer_source_actions(void*, wl_data_offer*, uint32_t) {}
void data_offer_action(void*, wl_data_offer*, uint32_t) {}
const wl_data_offer_listener kDataOfferListener{data_offer_offer, data_offer_source_actions,
                                                data_offer_action};

void device_data_offer(void* data, wl_data_device*, wl_data_offer* offer) {
    auto* st = static_cast<ClientState*>(data);
    st->data_offer_mimes.clear();
    wl_data_offer_add_listener(offer, &kDataOfferListener, st);
}
void device_enter(void*, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t,
                  wl_data_offer*) {}
void device_leave(void*, wl_data_device*) {}
void device_motion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void device_drop(void*, wl_data_device*) {}
void device_selection(void* data, wl_data_device*, wl_data_offer* offer) {
    auto* st = static_cast<ClientState*>(data);
    ++st->data_selections;
    st->data_offer = offer;
}
const wl_data_device_listener kDeviceListener{device_data_offer, device_enter, device_leave,
                                              device_motion,     device_drop,  device_selection};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_seat") == 0) {
        st->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 5));
    } else if (std::strcmp(interface, "wl_data_device_manager") == 0) {
        st->ddm = static_cast<wl_data_device_manager*>(
            wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3));
    } else if (std::strcmp(interface, "zwlr_data_control_manager_v1") == 0) {
        st->control = static_cast<zwlr_data_control_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface, 2));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

/// Ask `receive` for a mime through a pipe, pump the connection so the owning
/// side gets to write, and read the whole thing back.
std::string drain(wl_display* display, int read_fd) {
    wl_display_roundtrip(display); // the request reaches the server…
    wl_display_roundtrip(display); // …and the `send` event reaches the owner
    std::string out;
    char buf[128];
    ssize_t n = 0;
    while ((n = read(read_fd, buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    close(read_fd);
    return out;
}

std::string g_received_a;
std::string g_received_b;
// Snapshotted while direction A is current: setting the clipboard again in
// direction B replaces the data-control offer, mimes and all.
std::vector<std::string> g_control_mimes_a;

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState& st = g_client;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display);

    if (st.compositor == nullptr || st.seat == nullptr || st.ddm == nullptr ||
        st.control == nullptr) {
        wl_display_disconnect(display);
        return;
    }

    st.device = wl_data_device_manager_get_data_device(st.ddm, st.seat);
    wl_data_device_add_listener(st.device, &kDeviceListener, &st);
    st.control_device = zwlr_data_control_manager_v1_get_data_device(st.control, st.seat);
    zwlr_data_control_device_v1_add_listener(st.control_device, &kControlDeviceListener, &st);

    // The server takes keyboard focus here, which is what entitles this client
    // to see the ordinary clipboard at all.
    wl_surface* surface = wl_compositor_create_surface(st.compositor);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    // --- direction A ---
    wl_data_source* source = wl_data_device_manager_create_data_source(st.ddm);
    wl_data_source_add_listener(source, &kSourceListener, &st);
    wl_data_source_offer(source, kMimeA);
    wl_data_device_set_selection(st.device, source, 1);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    g_control_mimes_a = st.control_offer_mimes;
    if (st.control_offer != nullptr) {
        int pipe_fds[2];
        assert(pipe(pipe_fds) == 0);
        zwlr_data_control_offer_v1_receive(st.control_offer, kMimeA, pipe_fds[1]);
        close(pipe_fds[1]);
        g_received_a = drain(display, pipe_fds[0]);
    }

    // --- direction B ---
    zwlr_data_control_source_v1* control_source =
        zwlr_data_control_manager_v1_create_data_source(st.control);
    zwlr_data_control_source_v1_add_listener(control_source, &kControlSourceListener, &st);
    zwlr_data_control_source_v1_offer(control_source, kMimeB);
    zwlr_data_control_device_v1_set_selection(st.control_device, control_source);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (st.data_offer != nullptr) {
        int pipe_fds[2];
        assert(pipe(pipe_fds) == 0);
        wl_data_offer_receive(st.data_offer, kMimeB, pipe_fds[1]);
        close(pipe_fds[1]);
        g_received_b = drain(display, pipe_fds[0]);
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
    auto primary = luminaria::PrimarySelectionManager::create(*display, *seat);
    assert(primary.has_value());
    auto control = luminaria::DataControlManager::create(*display, *data_device, &*primary);
    assert(control.has_value());

    std::vector<std::vector<std::string>> selections;
    auto sc = data_device->selection_changed().connect(
        [&](luminaria::SelectionChange& e) { selections.push_back(e.mime_types); });

    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* surface = &e.surface;
        conns.push_back(e.surface.commit.connect(
            [&, surface](luminaria::SurfaceCommit&) { seat->set_keyboard_focus(surface); }));
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_client, fds[1]);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(5000);

    display->run();
    client_thread.join();

    // Direction A: the data-control device was told, and got the bytes.
    assert(g_client.control_selections >= 1);
    assert(g_control_mimes_a == std::vector<std::string>{kMimeA});
    assert(g_received_a == kPayloadA);

    // Direction B: the ordinary clipboard now belongs to a source that is not a
    // wl_data_source at all, and the focused client cannot tell the difference.
    assert(g_client.data_selections >= 1);
    assert(g_client.data_offer_mimes == std::vector<std::string>{kMimeB});
    assert(g_received_b == kPayloadB);

    // Two owners over the run: the wl_data_source, then the data-control one.
    assert(selections.size() >= 2);
    assert(selections[0] == std::vector<std::string>{kMimeA});
    assert(selections[1] == std::vector<std::string>{kMimeB});
    return 0;
}
