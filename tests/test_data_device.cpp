// Clipboard round-trip through wl_data_device_manager and through
// zwp_primary_selection_device_manager_v1. One in-process client owns the
// selection AND pastes it back: it sets a source, the server offers it to the
// focused client, the client pulls the data over a pipe, and the bytes it reads
// must be exactly what the source wrote. The compositor never touches them.
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

#include "primary-selection-unstable-v1-client-protocol.h"

#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"
#include "luminaria/data_device.hpp"
#include "luminaria/seat.hpp"

namespace {

constexpr const char* kMime = "text/plain;charset=utf-8";
constexpr const char* kClipboardText = "luminaria clipboard";
constexpr const char* kPrimaryText = "luminaria primary";

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    wl_data_device_manager* ddm = nullptr;
    zwp_primary_selection_device_manager_v1* psm = nullptr;
    wl_data_device* device = nullptr;
    zwp_primary_selection_device_v1* primary_device = nullptr;

    wl_data_offer* offer = nullptr;
    zwp_primary_selection_offer_v1* primary_offer = nullptr;
    bool offer_has_mime = false;
    bool primary_offer_has_mime = false;
    std::string pasted;
    std::string primary_pasted;
};

ClientState g_client;

// --- wl_data_source: we are the clipboard owner, so we serve the bytes ---
void source_target(void*, wl_data_source*, const char*) {}
void source_send(void*, wl_data_source*, const char* mime, int32_t fd) {
    if (mime != nullptr && std::strcmp(mime, kMime) == 0) {
        const size_t len = std::strlen(kClipboardText);
        [[maybe_unused]] const ssize_t written = write(fd, kClipboardText, len);
    }
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

// --- wl_data_offer: the paste side ---
void offer_offer(void* data, wl_data_offer*, const char* mime) {
    if (mime != nullptr && std::strcmp(mime, kMime) == 0) {
        static_cast<ClientState*>(data)->offer_has_mime = true;
    }
}
void offer_source_actions(void*, wl_data_offer*, uint32_t) {}
void offer_action(void*, wl_data_offer*, uint32_t) {}
const wl_data_offer_listener kOfferListener{offer_offer, offer_source_actions, offer_action};

// --- wl_data_device ---
void device_data_offer(void* data, wl_data_device*, wl_data_offer* offer) {
    wl_data_offer_add_listener(offer, &kOfferListener, data);
}
void device_enter(void*, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t,
                  wl_data_offer*) {}
void device_leave(void*, wl_data_device*) {}
void device_motion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void device_drop(void*, wl_data_device*) {}
void device_selection(void* data, wl_data_device*, wl_data_offer* offer) {
    static_cast<ClientState*>(data)->offer = offer;
}
const wl_data_device_listener kDeviceListener{device_data_offer, device_enter,
                                              device_leave,      device_motion,
                                              device_drop,       device_selection};

// --- primary selection ---
void primary_source_send(void*, zwp_primary_selection_source_v1*, const char* mime, int32_t fd) {
    if (mime != nullptr && std::strcmp(mime, kMime) == 0) {
        const size_t len = std::strlen(kPrimaryText);
        [[maybe_unused]] const ssize_t written = write(fd, kPrimaryText, len);
    }
    close(fd);
}
void primary_source_cancelled(void*, zwp_primary_selection_source_v1*) {}
const zwp_primary_selection_source_v1_listener kPrimarySourceListener{primary_source_send,
                                                                     primary_source_cancelled};

void primary_offer_offer(void* data, zwp_primary_selection_offer_v1*, const char* mime) {
    if (mime != nullptr && std::strcmp(mime, kMime) == 0) {
        static_cast<ClientState*>(data)->primary_offer_has_mime = true;
    }
}
const zwp_primary_selection_offer_v1_listener kPrimaryOfferListener{primary_offer_offer};

void primary_device_data_offer(void* data, zwp_primary_selection_device_v1*,
                               zwp_primary_selection_offer_v1* offer) {
    zwp_primary_selection_offer_v1_add_listener(offer, &kPrimaryOfferListener, data);
}
void primary_device_selection(void* data, zwp_primary_selection_device_v1*,
                              zwp_primary_selection_offer_v1* offer) {
    static_cast<ClientState*>(data)->primary_offer = offer;
}
const zwp_primary_selection_device_v1_listener kPrimaryDeviceListener{primary_device_data_offer,
                                                                     primary_device_selection};

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
    } else if (std::strcmp(interface, "wl_data_device_manager") == 0) {
        st->ddm = static_cast<wl_data_device_manager*>(
            wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3));
    } else if (std::strcmp(interface, "zwp_primary_selection_device_manager_v1") == 0) {
        st->psm = static_cast<zwp_primary_selection_device_manager_v1*>(wl_registry_bind(
            registry, name, &zwp_primary_selection_device_manager_v1_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

/// Ask the offer for `kMime` over a pipe and read what comes back.
std::string paste(wl_display* display, ClientState& st) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return {};
    }
    wl_data_offer_receive(st.offer, kMime, pipe_fds[1]);
    close(pipe_fds[1]);
    wl_display_roundtrip(display); // the source writes during this dispatch
    char buf[128] = {};
    const ssize_t n = read(pipe_fds[0], buf, sizeof(buf) - 1);
    close(pipe_fds[0]);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string{};
}

std::string paste_primary(wl_display* display, ClientState& st) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return {};
    }
    zwp_primary_selection_offer_v1_receive(st.primary_offer, kMime, pipe_fds[1]);
    close(pipe_fds[1]);
    wl_display_roundtrip(display);
    char buf[128] = {};
    const ssize_t n = read(pipe_fds[0], buf, sizeof(buf) - 1);
    close(pipe_fds[0]);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string{};
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

    if (st.compositor != nullptr && st.seat != nullptr && st.ddm != nullptr &&
        st.psm != nullptr) {
        st.device = wl_data_device_manager_get_data_device(st.ddm, st.seat);
        wl_data_device_add_listener(st.device, &kDeviceListener, &st);
        st.primary_device = zwp_primary_selection_device_manager_v1_get_device(st.psm, st.seat);
        zwp_primary_selection_device_v1_add_listener(st.primary_device, &kPrimaryDeviceListener,
                                                     &st);

        wl_surface* surface = wl_compositor_create_surface(st.compositor);
        wl_surface_commit(surface);    // the server focuses us on this commit
        wl_display_roundtrip(display);

        wl_data_source* source = wl_data_device_manager_create_data_source(st.ddm);
        wl_data_source_add_listener(source, &kSourceListener, &st);
        wl_data_source_offer(source, kMime);
        wl_data_device_set_selection(st.device, source, 1);
        wl_display_roundtrip(display); // data_offer + offer + selection come back

        if (st.offer != nullptr) {
            st.pasted = paste(display, st);
        }

        zwp_primary_selection_source_v1* primary_source =
            zwp_primary_selection_device_manager_v1_create_source(st.psm);
        zwp_primary_selection_source_v1_add_listener(primary_source, &kPrimarySourceListener,
                                                     &st);
        zwp_primary_selection_source_v1_offer(primary_source, kMime);
        zwp_primary_selection_device_v1_set_selection(st.primary_device, primary_source, 2);
        wl_display_roundtrip(display);

        if (st.primary_offer != nullptr) {
            st.primary_pasted = paste_primary(display, st);
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
    auto primary = luminaria::PrimarySelectionManager::create(*display, *seat);
    assert(primary.has_value());

    // Focus the client's surface: the selection is only offered to the focused
    // client, so this is what makes the clipboard reachable at all.
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
    timeout.arm(3000);

    display->run();
    client_thread.join();

    assert(g_client.offer != nullptr);
    assert(g_client.offer_has_mime);
    assert(g_client.pasted == kClipboardText);

    assert(g_client.primary_offer != nullptr);
    assert(g_client.primary_offer_has_mime);
    assert(g_client.primary_pasted == kPrimaryText);
    return 0;
}
