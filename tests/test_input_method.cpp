// zwp_input_method_v2, bridged to zwp_text_input_v3. One client plays both
// parts — the application typing into a field, and the input method converting
// for it — which is exactly the round trip that has to work for IBus or Fcitx:
//
//   app enables the field  ->  IM hears `activate` + state + `done`
//   IM commits a string    ->  app hears `commit_string` + `done`
//
// Nothing in between is the compositor's decision, so if this passes, an input
// method is talking to a real client through this library.
#include <cassert>
#include <cstdint>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "input-method-unstable-v2-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"

import luminaria.desktop;
import std;

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    zwp_text_input_manager_v3* ti_manager = nullptr;
    zwp_input_method_manager_v2* im_manager = nullptr;

    // What the input method heard.
    int activates = 0;
    int deactivates = 0;
    int im_dones = 0;
    std::uint32_t content_purpose = 0;

    // What the application heard.
    std::string commit_string;
    std::string preedit;
    int ti_dones = 0;
};

ClientState g_client;

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_seat") == 0) {
        st->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 5));
    } else if (std::strcmp(interface, "zwp_text_input_manager_v3") == 0) {
        st->ti_manager = static_cast<zwp_text_input_manager_v3*>(
            wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1));
    } else if (std::strcmp(interface, "zwp_input_method_manager_v2") == 0) {
        st->im_manager = static_cast<zwp_input_method_manager_v2*>(
            wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

// --- the application's side ---

void ti_enter(void*, zwp_text_input_v3*, wl_surface*) {}
void ti_leave(void*, zwp_text_input_v3*, wl_surface*) {}
void ti_preedit_string(void* data, zwp_text_input_v3*, const char* text, int32_t, int32_t) {
    static_cast<ClientState*>(data)->preedit = text != nullptr ? text : "";
}
void ti_commit_string(void* data, zwp_text_input_v3*, const char* text) {
    static_cast<ClientState*>(data)->commit_string = text != nullptr ? text : "";
}
void ti_delete_surrounding_text(void*, zwp_text_input_v3*, uint32_t, uint32_t) {}
void ti_done(void* data, zwp_text_input_v3*, uint32_t) {
    ++static_cast<ClientState*>(data)->ti_dones;
}
const zwp_text_input_v3_listener kTextInputListener{ti_enter,  ti_leave,
                                                    ti_preedit_string, ti_commit_string,
                                                    ti_delete_surrounding_text, ti_done};

// --- the input method's side ---

void im_activate(void* data, zwp_input_method_v2*) {
    ++static_cast<ClientState*>(data)->activates;
}
void im_deactivate(void* data, zwp_input_method_v2*) {
    ++static_cast<ClientState*>(data)->deactivates;
}
void im_surrounding_text(void*, zwp_input_method_v2*, const char*, uint32_t, uint32_t) {}
void im_text_change_cause(void*, zwp_input_method_v2*, uint32_t) {}
void im_content_type(void* data, zwp_input_method_v2*, uint32_t, uint32_t purpose) {
    static_cast<ClientState*>(data)->content_purpose = purpose;
}
void im_done(void* data, zwp_input_method_v2*) {
    ++static_cast<ClientState*>(data)->im_dones;
}
void im_unavailable(void*, zwp_input_method_v2*) {}
const zwp_input_method_v2_listener kInputMethodListener{
    im_activate,  im_deactivate,   im_surrounding_text, im_text_change_cause,
    im_content_type, im_done, im_unavailable};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState& st = g_client;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display);
    if (st.compositor == nullptr || st.seat == nullptr || st.ti_manager == nullptr ||
        st.im_manager == nullptr) {
        wl_display_disconnect(display);
        return;
    }

    wl_surface* surface = wl_compositor_create_surface(st.compositor);
    zwp_text_input_v3* ti = zwp_text_input_manager_v3_get_text_input(st.ti_manager, st.seat);
    zwp_text_input_v3_add_listener(ti, &kTextInputListener, &st);
    zwp_input_method_v2* im = zwp_input_method_manager_v2_get_input_method(st.im_manager, st.seat);
    zwp_input_method_v2_add_listener(im, &kInputMethodListener, &st);

    // The server focuses the keyboard on this surface when it commits, which is
    // what gives the text input somewhere to be focused.
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    // The application opens a text field.
    zwp_text_input_v3_enable(ti);
    zwp_text_input_v3_set_content_type(ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                                       ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL);
    zwp_text_input_v3_commit(ti);
    wl_display_roundtrip(display);

    // The input method answers with a conversion. `commit` echoes the number of
    // `done` events it has received, so it applies to the state it just read.
    const int done_count = st.im_dones;
    zwp_input_method_v2_set_preedit_string(im, "ni", 0, 2);
    zwp_input_method_v2_commit_string(im, "\xe4\xbd\xa0\xe5\xa5\xbd"); // 你好
    zwp_input_method_v2_commit(im, static_cast<uint32_t>(done_count));
    wl_display_roundtrip(display);

    // Closing the field deactivates the input method.
    zwp_text_input_v3_disable(ti);
    zwp_text_input_v3_commit(ti);
    wl_display_roundtrip(display);

    zwp_input_method_v2_destroy(im);
    zwp_text_input_v3_destroy(ti);
    wl_surface_destroy(surface);
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
    auto seat = luminaria::Seat::create(*display, "seat0");
    assert(seat.has_value());
    auto text_inputs = luminaria::TextInputManager::create(*display, *seat);
    assert(text_inputs.has_value());
    auto input_methods = luminaria::InputMethodManager::create(*display, *seat, *text_inputs);
    assert(input_methods.has_value());

    assert(input_methods->current() == nullptr);

    int new_input_methods = 0;
    auto nim = input_methods->new_input_method().connect(
        [&](luminaria::NewInputMethod&) { ++new_input_methods; });

    // Focus follows the seat's keyboard focus; give it somewhere to land.
    luminaria::Signal<luminaria::SurfaceCommit>::Connection on_commit;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface& surface = e.surface;
        on_commit = surface.commit.connect(
            [&, &surface = surface](luminaria::SurfaceCommit&) { seat->set_keyboard_focus(&surface); });
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

    assert(new_input_methods == 1);
    // The field opening reached the input method, content type and all.
    assert(g_client.activates == 1);
    assert(g_client.content_purpose == ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL);
    assert(g_client.im_dones >= 1);
    // ...and the conversion reached the application.
    assert(g_client.commit_string == "\xe4\xbd\xa0\xe5\xa5\xbd");
    assert(g_client.preedit == "ni");
    assert(g_client.ti_dones >= 1);
    // Closing the field turned the input method off again.
    assert(g_client.deactivates == 1);
    assert(input_methods->current() == nullptr); // the client destroyed it
    return 0;
}
