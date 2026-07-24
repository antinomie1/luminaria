// Phase 3 seat: keyboard focus + key routing, pointer enter + button. An
// in-process client binds wl_seat, gets a keyboard and pointer, and a surface.
// The server focuses that surface and injects a key + a pointer button; the
// client asserts it received keymap/enter/key and pointer enter/button.
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

#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"
#include "luminaria/seat.hpp"

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_pointer* pointer = nullptr;

    bool got_keymap = false;
    bool got_kb_enter = false;
    bool got_key = false;
    bool got_ptr_enter = false;
    bool got_button = false;
};

// --- keyboard listener ---
void kb_keymap(void* d, wl_keyboard*, uint32_t, int32_t fd, uint32_t) {
    static_cast<ClientState*>(d)->got_keymap = true;
    close(fd);
}
void kb_enter(void* d, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {
    static_cast<ClientState*>(d)->got_kb_enter = true;
}
void kb_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
void kb_key(void* d, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t) {
    static_cast<ClientState*>(d)->got_key = true;
}
void kb_modifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void kb_repeat(void*, wl_keyboard*, int32_t, int32_t) {}
const wl_keyboard_listener kKeyboardListener{kb_keymap,     kb_enter,  kb_leave,
                                             kb_key,        kb_modifiers, kb_repeat};

// --- pointer listener ---
void ptr_enter(void* d, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {
    static_cast<ClientState*>(d)->got_ptr_enter = true;
}
void ptr_leave(void*, wl_pointer*, uint32_t, wl_surface*) {}
void ptr_motion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void ptr_button(void* d, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) {
    static_cast<ClientState*>(d)->got_button = true;
}
void ptr_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
void ptr_frame(void*, wl_pointer*) {}
const wl_pointer_listener kPointerListener{ptr_enter, ptr_leave, ptr_motion, ptr_button,
                                           ptr_axis,  ptr_frame};

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
        st->keyboard = wl_seat_get_keyboard(st->seat);
        wl_keyboard_add_listener(st->keyboard, &kKeyboardListener, st);
        st->pointer = wl_seat_get_pointer(st->seat);
        wl_pointer_add_listener(st->pointer, &kPointerListener, st);
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

ClientState g_client; // lives past the client thread so main can assert on it

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState& st = g_client;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display); // globals + seat/keyboard/pointer bound

    if (st.compositor != nullptr && st.seat != nullptr) {
        wl_surface* surface = wl_compositor_create_surface(st.compositor);
        wl_surface_commit(surface);    // server focuses + injects input on this commit
        wl_display_roundtrip(display); // flush; server processes commit
        wl_display_roundtrip(display); // receive the input events
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

    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> commit_conns;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* surface = &e.surface;
        commit_conns.push_back(e.surface.commit.connect([&, surface](luminaria::SurfaceCommit&) {
            seat->set_keyboard_focus(surface);
            seat->notify_key(30 /*KEY_A*/, true);
            seat->pointer_enter(*surface, 1.0, 2.0);
            seat->pointer_button(272 /*BTN_LEFT*/, true);
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

    assert(g_client.got_keymap);
    assert(g_client.got_kb_enter);
    assert(g_client.got_key);
    assert(g_client.got_ptr_enter);
    assert(g_client.got_button);
    return 0;
}
