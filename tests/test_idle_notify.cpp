// ext_idle_notifier_v1. Two notifications with the same timeout, one ordinary
// and one made with get_input_idle_notification (version 2), differ in exactly
// one way: an idle inhibitor holds the first off and the second keeps counting.
//
// The whole protocol is timers, so the test is a server-driven timeline: arm a
// checkpoint, look at what has and has not idled, move on. Timeouts are 30 ms
// and the checkpoints are 150 ms apart, which is four timer periods of slack.
#include <cassert>
#include <cstddef>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "ext-idle-notify-v1-client-protocol.h"

import luminaria;
import std;

namespace {

constexpr std::uint32_t kTimeoutMs = 30;
constexpr unsigned kCheckpointMs = 150;

struct ClientState {
    wl_seat* seat = nullptr;
    ext_idle_notifier_v1* notifier = nullptr;
    // What the wire actually delivered, in order, for each notification.
    std::vector<const char*> ordinary;
    std::vector<const char*> input_only;
};

ClientState g_client;

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t version) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_seat") == 0) {
        st->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    } else if (std::strcmp(interface, "ext_idle_notifier_v1") == 0) {
        // The global must advertise version 2, or get_input_idle_notification
        // is not reachable at all.
        assert(version >= 2);
        st->notifier = static_cast<ext_idle_notifier_v1*>(
            wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, 2));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

void ordinary_idled(void*, ext_idle_notification_v1*) { g_client.ordinary.push_back("idled"); }
void ordinary_resumed(void*, ext_idle_notification_v1*) { g_client.ordinary.push_back("resumed"); }
const ext_idle_notification_v1_listener kOrdinaryListener{ordinary_idled, ordinary_resumed};

void input_idled(void*, ext_idle_notification_v1*) { g_client.input_only.push_back("idled"); }
void input_resumed(void*, ext_idle_notification_v1*) { g_client.input_only.push_back("resumed"); }
const ext_idle_notification_v1_listener kInputListener{input_idled, input_resumed};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState& st = g_client;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display);
    if (st.seat == nullptr || st.notifier == nullptr) {
        wl_display_disconnect(display);
        return;
    }

    ext_idle_notification_v1* a =
        ext_idle_notifier_v1_get_idle_notification(st.notifier, kTimeoutMs, st.seat);
    ext_idle_notification_v1_add_listener(a, &kOrdinaryListener, nullptr);
    ext_idle_notification_v1* b =
        ext_idle_notifier_v1_get_input_idle_notification(st.notifier, kTimeoutMs, st.seat);
    ext_idle_notification_v1_add_listener(b, &kInputListener, nullptr);
    wl_display_roundtrip(display);

    // Sit on the socket until the server is done with us.
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

} // namespace

int main() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto seat = luminaria::Seat::create(*display);
    assert(seat.has_value());
    auto notifier = luminaria::IdleNotifier::create(*display, display->event_loop());
    assert(notifier.has_value());

    assert(notifier->notification_count() == 0);
    assert(!notifier->inhibited());

    // Server-side view of every transition, to cross-check the wire events.
    std::vector<bool> transitions;
    auto sc = notifier->state_change().connect(
        [&](luminaria::IdleStateChange& e) { transitions.push_back(e.idled); });

    luminaria::IdleNotification* ordinary = nullptr;
    luminaria::IdleNotification* input_only = nullptr;
    auto nn = notifier->new_notification().connect([&](luminaria::NewIdleNotification& e) {
        (e.notification.input_only() ? input_only : ordinary) = &e.notification;
    });

    // --- the timeline ---
    //
    // The inhibitor is already held when the client connects, so the ordinary
    // notification is held off from birth and the input-only one is not. That
    // makes every event below unambiguous; arming the inhibitor later would
    // race the 30 ms timeouts that start the moment each object is created.
    notifier->set_inhibited(true);

    bool inhibited_only_input_idled = false;
    bool both_idled_after_release = false;
    bool both_resumed_after_activity = false;
    std::size_t live_at_end = 0;

    luminaria::EventSource checkpoint2;
    luminaria::EventSource checkpoint1 = display->event_loop().add_timer([&] {
        assert(ordinary != nullptr && input_only != nullptr);
        assert(ordinary->timeout_ms() == kTimeoutMs);
        assert(ordinary->input_only() == false && input_only->input_only() == true);
        // Held off vs. deliberately blind to the inhibitor.
        inhibited_only_input_idled =
            !ordinary->idled() && input_only->idled() && notifier->idled_count() == 1;
        // Inhibitor goes away: the ordinary one starts counting from now.
        notifier->set_inhibited(false);
        checkpoint2.arm(kCheckpointMs);
    });

    checkpoint2 = display->event_loop().add_timer([&] {
        both_idled_after_release =
            ordinary->idled() && input_only->idled() && notifier->idled_count() == 2;
        // The user touches something: everything resumes, synchronously.
        notifier->notify_activity();
        both_resumed_after_activity =
            !ordinary->idled() && !input_only->idled() && notifier->idled_count() == 0;
        live_at_end = notifier->notification_count();
        display->terminate();
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_client, fds[1]);
    checkpoint1.arm(kCheckpointMs);
    auto guard = display->event_loop().add_timer([&] { display->terminate(); });
    guard.arm(5000);

    display->run();
    // The client is parked in wl_display_dispatch; dropping its connection is
    // what lets it fall out and be joined. wl_client_destroy owns fds[0].
    wl_client_destroy(client);
    client_thread.join();

    assert(live_at_end == 2);
    // Destroying the client took its notifications with it.
    assert(notifier->notification_count() == 0);
    assert(inhibited_only_input_idled);
    assert(both_idled_after_release);
    assert(both_resumed_after_activity);

    // The wire agrees with the server's own accounting. The ordinary one idled
    // exactly once — it was held off through the first checkpoint — while the
    // input-only one idled straight away; both then resumed on activity.
    assert((g_client.ordinary == std::vector<const char*>{"idled", "resumed"}));
    assert((g_client.input_only == std::vector<const char*>{"idled", "resumed"}));
    // idled(input), idled(ordinary), resumed, resumed — no spurious flapping.
    assert(transitions.size() == 4);
    assert((transitions == std::vector<bool>{true, true, false, false}));
    return 0;
}
