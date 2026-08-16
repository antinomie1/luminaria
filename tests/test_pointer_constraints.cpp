// zwp_pointer_constraints_v1. Three properties worth guarding:
//   * a constraint is inert until the compositor activates it, and activation
//     is refused for a surface that does not hold pointer focus,
//   * losing pointer focus deactivates it without the compositor lifting a
//     finger — that is what stops a client keeping the mouse forever,
//   * set_cursor_position_hint is double-buffered, so it lands on commit and
//     not before.
#include <cassert>
#include <cstddef>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "pointer-constraints-unstable-v1-client-protocol.h"

import luminaria;
import std;

namespace {

constexpr double kHintX = 12.5;
constexpr double kHintY = 34.25;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    zwp_pointer_constraints_v1* constraints = nullptr;
    zwp_locked_pointer_v1* lock = nullptr;

    int locked = 0;
    int unlocked = 0;
};

ClientState g_client;

void on_locked(void* data, zwp_locked_pointer_v1*) {
    ++static_cast<ClientState*>(data)->locked;
}
void on_unlocked(void* data, zwp_locked_pointer_v1*) {
    ++static_cast<ClientState*>(data)->unlocked;
}
const zwp_locked_pointer_v1_listener kLockListener{on_locked, on_unlocked};

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
    } else if (std::strcmp(interface, "zwp_pointer_constraints_v1") == 0) {
        st->constraints = static_cast<zwp_pointer_constraints_v1*>(
            wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
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

    if (st.compositor == nullptr || st.seat == nullptr || st.constraints == nullptr) {
        wl_display_disconnect(display);
        return;
    }

    wl_surface* surface = wl_compositor_create_surface(st.compositor);
    st.lock = zwp_pointer_constraints_v1_lock_pointer(st.constraints, surface, st.pointer, nullptr,
                                                      ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_locked_pointer_v1_add_listener(st.lock, &kLockListener, &st);
    zwp_locked_pointer_v1_set_cursor_position_hint(st.lock, wl_fixed_from_double(kHintX),
                                                   wl_fixed_from_double(kHintY));

    // Commit 1: the server tries to activate with no pointer focus (refused),
    // then gives focus and activates for real. The hint lands on this commit.
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    // Commit 2: the server moves the pointer off the surface, which must
    // deactivate the lock all by itself.
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
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
    auto constraints = luminaria::PointerConstraints::create(*display, *seat);
    assert(constraints.has_value());

    int new_constraints = 0;
    bool right_type = false;
    bool right_lifetime = false;
    bool no_region = false;
    bool refused_without_focus = false;
    bool active_after_focus = false;
    bool hint_before_commit = true;
    bool hint_after_commit = false;
    double hint_x = 0.0, hint_y = 0.0;
    bool deactivated_by_focus_loss = false;
    luminaria::PointerConstraint* seen = nullptr;

    auto nc = constraints->new_constraint().connect([&](luminaria::NewPointerConstraint& e) {
        ++new_constraints;
        seen = &e.constraint;
        right_type = e.constraint.type() == luminaria::PointerConstraintType::locked;
        right_lifetime =
            e.constraint.lifetime() == luminaria::PointerConstraintLifetime::persistent;
        no_region = !e.constraint.has_region();
        // The hint arrived in the same batch as the lock request but has not
        // been committed yet, so it must not be visible.
        hint_before_commit = e.constraint.has_cursor_position_hint();
    });

    int commits = 0;
    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* surface = &e.surface;
        conns.push_back(e.surface.commit.connect([&, surface](luminaria::SurfaceCommit&) {
            if (++commits == 1) {
                assert(seen != nullptr);
                seen->activate();
                refused_without_focus = !seen->active(); // no pointer focus yet
                seat->pointer_enter(*surface, 0.0, 0.0);
                seen->activate();
                active_after_focus = seen->active();
                return;
            }
            // The constraint applied its own pending state on this commit
            // before we ran, so the hint is current now.
            hint_after_commit = seen->has_cursor_position_hint();
            hint_x = seen->cursor_hint_x();
            hint_y = seen->cursor_hint_y();
            seat->pointer_clear_focus();
            deactivated_by_focus_loss =
                !seen->active() && constraints->active_constraint() == nullptr;
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

    assert(new_constraints == 1);
    assert(right_type && right_lifetime && no_region);
    assert(!hint_before_commit);
    assert(refused_without_focus);
    assert(active_after_focus);
    assert(hint_after_commit && hint_x == kHintX && hint_y == kHintY);
    assert(deactivated_by_focus_loss);

    assert(g_client.locked == 1);
    assert(g_client.unlocked == 1);
    return 0;
}
