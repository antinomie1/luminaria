// xdg_activation_v1: a client asks for a token, hands it back on `activate`,
// and the compositor is told which surface wants focus and what justified it.
// The token is single-use — the second attempt, and an invented one, both come
// back invalid, which is the whole anti-focus-stealing property.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "xdg-activation-v1-client-protocol.h"

import luminaria;

namespace {

constexpr uint32_t kSerial = 42;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    xdg_activation_v1* activation = nullptr;
    wl_surface* requester = nullptr;
    wl_surface* target = nullptr;
    std::string token;
};

ClientState g_client;

void token_done(void* data, xdg_activation_token_v1*, const char* token) {
    static_cast<ClientState*>(data)->token = token;
}
const xdg_activation_token_v1_listener kTokenListener{token_done};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_seat") == 0) {
        st->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 5));
    } else if (std::strcmp(interface, "xdg_activation_v1") == 0) {
        st->activation = static_cast<xdg_activation_v1*>(
            wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
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

    if (st.compositor == nullptr || st.seat == nullptr || st.activation == nullptr) {
        wl_display_disconnect(display);
        return;
    }

    st.requester = wl_compositor_create_surface(st.compositor);
    st.target = wl_compositor_create_surface(st.compositor);
    wl_display_roundtrip(display);

    xdg_activation_token_v1* token = xdg_activation_v1_get_activation_token(st.activation);
    xdg_activation_token_v1_add_listener(token, &kTokenListener, &st);
    xdg_activation_token_v1_set_serial(token, kSerial, st.seat);
    xdg_activation_token_v1_set_app_id(token, "org.luminaria.target");
    xdg_activation_token_v1_set_surface(token, st.requester);
    xdg_activation_token_v1_commit(token);
    wl_display_roundtrip(display);
    assert(!st.token.empty());

    xdg_activation_v1_activate(st.activation, st.token.c_str(), st.target);
    wl_display_roundtrip(display);
    // Replaying it must not work.
    xdg_activation_v1_activate(st.activation, st.token.c_str(), st.target);
    wl_display_roundtrip(display);
    // Nor must one the compositor never issued.
    xdg_activation_v1_activate(st.activation, "0123456789abcdef0123456789abcdef", st.target);
    wl_display_roundtrip(display);

    xdg_activation_token_v1_destroy(token);
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
    auto activation = luminaria::XdgActivation::create(*display);
    assert(activation.has_value());

    int token_requests = 0;
    uint32_t seen_serial = 0;
    std::string seen_token_app_id;
    bool token_named_seat = false;
    bool token_named_surface = false;

    auto tconn = activation->new_token().connect([&](luminaria::ActivationTokenRequest& r) {
        ++token_requests;
        seen_serial = r.serial;
        seen_token_app_id = r.app_id;
        token_named_seat = r.seat != nullptr;
        token_named_surface = r.surface != nullptr;
    });

    std::vector<bool> validity;
    std::string seen_activate_app_id;
    bool carried_requester = false;
    auto aconn = activation->request_activate().connect([&](luminaria::ActivationRequest& r) {
        validity.push_back(r.token_valid);
        if (r.token_valid) {
            seen_activate_app_id = r.app_id;
            carried_requester = r.requesting_surface != nullptr;
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

    assert(token_requests == 1);
    assert(seen_serial == kSerial);
    assert(seen_token_app_id == "org.luminaria.target");
    assert(token_named_seat);
    assert(token_named_surface);

    assert(validity.size() == 3);
    assert(validity[0]);   // the token works once…
    assert(!validity[1]);  // …and only once
    assert(!validity[2]);  // an invented token is worth nothing
    assert(seen_activate_app_id == "org.luminaria.target");
    assert(carried_requester);
    return 0;
}
