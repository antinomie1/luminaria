// Per-output scale and transform, as a client sees them: wl_output must report
// the mode, the scale and the rotation, and xdg-output must report the LOGICAL
// size — mode divided by scale, axes swapped by the rotation. Tools that place
// screenshots on a canvas read the latter.
#include <cassert>
#include <cstddef>
#include <cstring>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "xdg-output-unstable-v1-client-protocol.h"

import luminaria;

namespace {

struct ClientState {
    wl_output* output = nullptr;
    zxdg_output_manager_v1* xdg_manager = nullptr;
    int32_t transform = -1;
    int32_t scale = -1;
    int32_t mode_w = 0, mode_h = 0;
    int32_t logical_w = 0, logical_h = 0;
    int32_t logical_x = -1, logical_y = -1;
    int dones = 0;
};

void out_geometry(void* data, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*,
                  const char*, int32_t transform) {
    static_cast<ClientState*>(data)->transform = transform;
}
void out_mode(void* data, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t) {
    if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) {
        auto* st = static_cast<ClientState*>(data);
        st->mode_w = w;
        st->mode_h = h;
    }
}
void out_done(void* data, wl_output*) { ++static_cast<ClientState*>(data)->dones; }
void out_scale(void* data, wl_output*, int32_t scale) {
    static_cast<ClientState*>(data)->scale = scale;
}
void out_name(void*, wl_output*, const char*) {}
void out_description(void*, wl_output*, const char*) {}
const wl_output_listener kOutput{out_geometry, out_mode, out_done,
                                 out_scale,    out_name, out_description};

void xdg_logical_position(void* data, zxdg_output_v1*, int32_t x, int32_t y) {
    auto* st = static_cast<ClientState*>(data);
    st->logical_x = x;
    st->logical_y = y;
}
void xdg_logical_size(void* data, zxdg_output_v1*, int32_t w, int32_t h) {
    auto* st = static_cast<ClientState*>(data);
    st->logical_w = w;
    st->logical_h = h;
}
void xdg_done(void*, zxdg_output_v1*) {}
void xdg_name(void*, zxdg_output_v1*, const char*) {}
void xdg_description(void*, zxdg_output_v1*, const char*) {}
const zxdg_output_v1_listener kXdgOutput{xdg_logical_position, xdg_logical_size, xdg_done, xdg_name,
                                         xdg_description};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "wl_output") == 0) {
        st->output = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, 4));
    } else if (std::strcmp(iface, "zxdg_output_manager_v1") == 0) {
        st->xdg_manager = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, 3));
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
    assert(st.output != nullptr && st.xdg_manager != nullptr);

    wl_output_add_listener(st.output, &kOutput, &st);
    zxdg_output_v1* xdg = zxdg_output_manager_v1_get_xdg_output(st.xdg_manager, st.output);
    zxdg_output_v1_add_listener(xdg, &kXdgOutput, &st);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    // The mode is untouched by scale or rotation…
    assert(st.mode_w == 800 && st.mode_h == 600);
    // …but the client is told about both.
    assert(st.scale == 2);
    assert(st.transform == WL_OUTPUT_TRANSFORM_90);
    // Logical: 800x600 rotated is 600x800, halved by the scale.
    assert(st.logical_w == 300 && st.logical_h == 400);
    assert(st.logical_x == 100 && st.logical_y == 50);
    assert(st.dones > 0);

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
    auto output = luminaria::OutputGlobal::create(*display, 800, 600);
    assert(output.has_value());
    output->set_scale(2);
    output->set_transform(luminaria::Transform::rotate_90);
    output->set_logical_position(100, 50);
    assert(output->logical_width() == 300);
    assert(output->logical_height() == 400);

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
    return 0;
}
