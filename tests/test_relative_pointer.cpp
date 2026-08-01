// zwp_relative_pointer_v1: the compositor pushes a motion delta and only the
// client holding POINTER FOCUS sees it. Both pairs of deltas — accelerated and
// raw — and the split 64-bit timestamp have to survive the wire intact, since a
// game reading the unaccelerated pair is the whole reason the protocol exists.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "relative-pointer-unstable-v1-client-protocol.h"

import luminaria;

namespace {

constexpr std::uint64_t kTimeUs = 0x0000'0002'DEAD'BEEFULL; // straddles 32 bits
constexpr double kDx = 3.5;
constexpr double kDy = -2.25;
constexpr double kDxUnaccel = 4.0;
constexpr double kDyUnaccel = -3.0;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    zwp_relative_pointer_manager_v1* manager = nullptr;
    zwp_relative_pointer_v1* relative = nullptr;

    int motions = 0;
    std::uint64_t time_us = 0;
    double dx = 0.0, dy = 0.0, dx_unaccel = 0.0, dy_unaccel = 0.0;
};

ClientState g_client;

void relative_motion(void* data, zwp_relative_pointer_v1*, uint32_t utime_hi, uint32_t utime_lo,
                     wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) {
    auto* st = static_cast<ClientState*>(data);
    ++st->motions;
    st->time_us = (static_cast<std::uint64_t>(utime_hi) << 32) | utime_lo;
    st->dx = wl_fixed_to_double(dx);
    st->dy = wl_fixed_to_double(dy);
    st->dx_unaccel = wl_fixed_to_double(dx_unaccel);
    st->dy_unaccel = wl_fixed_to_double(dy_unaccel);
}
const zwp_relative_pointer_v1_listener kRelativeListener{relative_motion};

void ptr_enter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {}
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

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_seat") == 0) {
        st->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 5));
        st->pointer = wl_seat_get_pointer(st->seat);
        wl_pointer_add_listener(st->pointer, &kPointerListener, st);
    } else if (std::strcmp(interface, "zwp_relative_pointer_manager_v1") == 0) {
        st->manager = static_cast<zwp_relative_pointer_manager_v1*>(
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
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

    if (st.compositor != nullptr && st.seat != nullptr && st.manager != nullptr) {
        st.relative = zwp_relative_pointer_manager_v1_get_relative_pointer(st.manager, st.pointer);
        zwp_relative_pointer_v1_add_listener(st.relative, &kRelativeListener, &st);

        wl_surface* surface = wl_compositor_create_surface(st.compositor);
        wl_surface_commit(surface); // the server injects the motion here
        wl_display_roundtrip(display);
        wl_display_roundtrip(display);
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
    auto relative = luminaria::RelativePointerManager::create(*display, *seat);
    assert(relative.has_value());

    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* surface = &e.surface;
        conns.push_back(e.surface.commit.connect([&, surface](luminaria::SurfaceCommit&) {
            // Nobody has pointer focus yet: this one must go nowhere at all.
            relative->send_motion(kTimeUs, 99.0, 99.0, 99.0, 99.0);
            seat->pointer_enter(*surface, 0.0, 0.0);
            relative->send_motion(kTimeUs, kDx, kDy, kDxUnaccel, kDyUnaccel);
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

    // Exactly one: the pre-focus motion was dropped, which is the point.
    assert(g_client.motions == 1);
    assert(g_client.time_us == kTimeUs);
    assert(g_client.dx == kDx && g_client.dy == kDy);
    assert(g_client.dx_unaccel == kDxUnaccel && g_client.dy_unaccel == kDyUnaccel);
    return 0;
}
