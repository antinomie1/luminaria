// wp_cursor_shape_v1: a client names the cursor it wants and the compositor
// gets the request with the matching theme name.
#include <cstdint>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "cursor-shape-v1-client-protocol.h"

import luminaria;

namespace {

struct ClientState {
    wl_seat* seat = nullptr;
    wp_cursor_shape_manager_v1* manager = nullptr;
};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "wl_seat") == 0) {
        st->seat = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, 5));
    } else if (std::strcmp(iface, "wp_cursor_shape_manager_v1") == 0) {
        st->manager = static_cast<wp_cursor_shape_manager_v1*>(
            wl_registry_bind(reg, name, &wp_cursor_shape_manager_v1_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState st;
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistry, &st);
    wl_display_roundtrip(display);
    assert(st.seat != nullptr && st.manager != nullptr);

    wl_pointer* pointer = wl_seat_get_pointer(st.seat);
    wp_cursor_shape_device_v1* device =
        wp_cursor_shape_manager_v1_get_pointer(st.manager, pointer);
    wp_cursor_shape_device_v1_set_shape(device, 42,
                                        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED);
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

std::string g_name;
std::uint32_t g_serial = 0;
bool g_had_pointer = false;

} // namespace

int main() {
    // The name table is what a theme lookup actually uses; check both ends.
    assert(std::strcmp(luminaria::cursor_shape_name(1), "default") == 0);
    assert(std::strcmp(luminaria::cursor_shape_name(9), "text") == 0);
    assert(std::strcmp(luminaria::cursor_shape_name(36), "all-resize") == 0);
    assert(luminaria::cursor_shape_name(0) == nullptr);
    assert(luminaria::cursor_shape_name(37) == nullptr);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto seat = luminaria::Seat::create(*display);
    assert(seat.has_value());
    auto cursor_shape = luminaria::CursorShapeManager::create(*display);
    assert(cursor_shape.has_value());

    auto conn = cursor_shape->request().connect([&](luminaria::CursorShapeRequest& e) {
        g_name = e.name;
        g_serial = e.serial;
        g_had_pointer = e.pointer != nullptr;
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx dc{{}, &*display};
    dc.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &dc.listener);

    std::thread client_thread(run_client, fds[1]);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(3000);
    display->run();
    client_thread.join();

    assert(g_name == "not-allowed");
    assert(g_serial == 42);
    assert(g_had_pointer);
    return 0;
}
