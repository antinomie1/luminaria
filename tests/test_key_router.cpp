// The keyboard between the compositor and the focused client, end to end:
//
//  - `KeyRouter::route` gives a binding the press first; a key it keeps never
//    reaches the client, release included, and every other key does;
//  - `Seat::release_keys` releases exactly the keys the client was told are
//    down and clears the modifiers — what a VT switch needs;
//  - `Seat::follow` adopts an input source's capabilities at once and follows
//    later changes, advertising touch only when asked to.
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

constexpr std::uint32_t kKeyA = 30;
constexpr std::uint32_t kKeyQ = 16;

struct KeySeen {
    std::uint32_t key;
    std::uint32_t state;
};

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    wl_keyboard* keyboard = nullptr;
    std::vector<KeySeen> keys;
    std::uint32_t last_depressed = 0xffff;
    std::vector<std::uint32_t> capabilities;
};

ClientState g_client;

void kb_keymap(void*, wl_keyboard*, std::uint32_t, int fd, std::uint32_t) { close(fd); }
void kb_enter(void*, wl_keyboard*, std::uint32_t, wl_surface*, wl_array*) {}
void kb_leave(void*, wl_keyboard*, std::uint32_t, wl_surface*) {}
void kb_key(void*, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t key,
            std::uint32_t state) {
    g_client.keys.push_back({key, state});
}
void kb_modifiers(void*, wl_keyboard*, std::uint32_t, std::uint32_t depressed, std::uint32_t,
                  std::uint32_t, std::uint32_t) {
    g_client.last_depressed = depressed;
}
void kb_repeat(void*, wl_keyboard*, std::int32_t, std::int32_t) {}
const wl_keyboard_listener kKeyboard{kb_keymap, kb_enter,     kb_leave,
                                     kb_key,    kb_modifiers, kb_repeat};

void seat_caps(void*, wl_seat* seat, std::uint32_t caps) {
    g_client.capabilities.push_back(caps);
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && g_client.keyboard == nullptr) {
        g_client.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_client.keyboard, &kKeyboard, nullptr);
    }
}
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener kSeat{seat_caps, seat_name};

void registry_global(void*, wl_registry* reg, std::uint32_t name, const char* iface,
                     std::uint32_t) {
    if (std::strcmp(iface, "wl_compositor") == 0) {
        g_client.compositor =
            static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(iface, "wl_seat") == 0) {
        g_client.seat = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, 5));
        wl_seat_add_listener(g_client.seat, &kSeat, nullptr);
    }
}
void registry_global_remove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    assert(display != nullptr);
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistry, nullptr);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); // capabilities, then the keyboard
    wl_surface* surface = wl_compositor_create_surface(g_client.compositor);
    wl_surface_commit(surface);
    wl_display_flush(display);
    while (wl_display_dispatch(display) >= 0) {
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

/// An input source the test drives by hand.
class FakeSource final : public luminaria::InputSource {
public:
    luminaria::Signal<luminaria::InputEvent>& input() noexcept override { return input_; }
    const std::string& keymap() const noexcept override { return keymap_; }
    luminaria::InputCapabilities capabilities() const noexcept override { return caps_; }

    luminaria::InputCapabilities caps_{.keyboard = true, .pointer = false, .touch = true};

private:
    luminaria::Signal<luminaria::InputEvent> input_;
    std::string keymap_;
};

luminaria::KeyEvent key(std::uint32_t code, bool pressed) {
    return {.keycode = code, .pressed = pressed};
}

} // namespace

int main() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto globals = luminaria::CoreGlobals::create(*display);
    assert(globals.has_value());
    luminaria::Seat& seat = globals->seat();

    // Followed before any client: keyboard yes, pointer no, touch masked off.
    FakeSource source;
    seat.follow(source);

    luminaria::Signal<luminaria::SurfaceCommit>::Connection on_commit;
    auto new_surface = globals->compositor().new_surface().connect([&](luminaria::NewSurface& e) {
        on_commit = e.surface.commit.connect([&seat](luminaria::SurfaceCommit& c) {
            seat.set_keyboard_focus(&c.surface);
        });
    });

    luminaria::KeyRouter router;
    int bindings_run = 0;
    auto binding = [&](const luminaria::KeyEvent& e) {
        ++bindings_run;
        return e.keycode == kKeyQ;
    };

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    auto type = display->event_loop().add_timer([&] {
        assert(seat.keyboard_focus().valid());
        router.route(seat, key(kKeyA, true), binding);  // the client's
        router.route(seat, key(kKeyQ, true), binding);  // kept
        router.route(seat, key(kKeyQ, false), binding); // kept too, unasked
        seat.notify_modifiers(1, 0, 0, 0);
        // The VT goes away with A still down: its release arrives elsewhere.
        seat.release_keys();
        seat.release_keys(); // nothing left to release
        // Capabilities follow the source; touch stays masked.
        source.caps_ = {.keyboard = true, .pointer = true, .touch = true};
        luminaria::InputEvent change = source.caps_;
        source.input().emit(change);
    });
    type.arm(150);

    std::thread client_thread(run_client, fds[1]);
    auto stop = display->event_loop().add_timer([&] { display->terminate(); });
    stop.arm(400);
    display->run();
    wl_client_destroy(client);
    client_thread.join();

    // Only presses are offered to the binding.
    assert(bindings_run == 2);
    // A down and up — the up from release_keys() — and nothing of Q.
    assert(g_client.keys.size() == 2);
    assert(g_client.keys[0].key == kKeyA &&
           g_client.keys[0].state == WL_KEYBOARD_KEY_STATE_PRESSED);
    assert(g_client.keys[1].key == kKeyA &&
           g_client.keys[1].state == WL_KEYBOARD_KEY_STATE_RELEASED);
    assert(g_client.last_depressed == 0);
    assert(!g_client.capabilities.empty());
    assert(g_client.capabilities.front() == WL_SEAT_CAPABILITY_KEYBOARD);
    assert(g_client.capabilities.back() ==
           (WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER));
    return 0;
}
