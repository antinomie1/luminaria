// zwp_text_input_v3, both directions. The client's state is double-buffered, so
// nothing set before `commit` may be visible; ours is too, so nothing sent
// before `done` may reach the client. Focus is not the client's to pick — it
// follows the seat's keyboard focus — and the done serial must equal the number
// of commits received, which is how the client tells a stale answer from a
// current one.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "text-input-unstable-v3-client-protocol.h"

import luminaria;
import std;

namespace {

constexpr const char* kSurrounding = "hello";
constexpr const char* kPreedit = "ni";
constexpr const char* kCommitString = "你好";

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    zwp_text_input_manager_v3* manager = nullptr;
    zwp_text_input_v3* text_input = nullptr;

    int enters = 0;
    int leaves = 0;
    std::string preedit;
    int32_t preedit_begin = 0, preedit_end = 0;
    std::string commit_string;
    uint32_t delete_before = 0, delete_after = 0;
    int dones = 0;
    uint32_t last_serial = 0;
};

ClientState g_client;

void ti_enter(void* data, zwp_text_input_v3*, wl_surface*) {
    ++static_cast<ClientState*>(data)->enters;
}
void ti_leave(void* data, zwp_text_input_v3*, wl_surface*) {
    ++static_cast<ClientState*>(data)->leaves;
}
void ti_preedit_string(void* data, zwp_text_input_v3*, const char* text, int32_t begin,
                       int32_t end) {
    auto* st = static_cast<ClientState*>(data);
    st->preedit = text != nullptr ? text : "";
    st->preedit_begin = begin;
    st->preedit_end = end;
}
void ti_commit_string(void* data, zwp_text_input_v3*, const char* text) {
    static_cast<ClientState*>(data)->commit_string = text != nullptr ? text : "";
}
void ti_delete_surrounding_text(void* data, zwp_text_input_v3*, uint32_t before, uint32_t after) {
    auto* st = static_cast<ClientState*>(data);
    st->delete_before = before;
    st->delete_after = after;
}
void ti_done(void* data, zwp_text_input_v3*, uint32_t serial) {
    auto* st = static_cast<ClientState*>(data);
    ++st->dones;
    st->last_serial = serial;
}
const zwp_text_input_v3_listener kTextInputListener{ti_enter,
                                                    ti_leave,
                                                    ti_preedit_string,
                                                    ti_commit_string,
                                                    ti_delete_surrounding_text,
                                                    ti_done};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_seat") == 0) {
        st->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 5));
    } else if (std::strcmp(interface, "zwp_text_input_manager_v3") == 0) {
        st->manager = static_cast<zwp_text_input_manager_v3*>(
            wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1));
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

    if (st.compositor == nullptr || st.seat == nullptr || st.manager == nullptr) {
        wl_display_disconnect(display);
        return;
    }

    st.text_input = zwp_text_input_manager_v3_get_text_input(st.manager, st.seat);
    zwp_text_input_v3_add_listener(st.text_input, &kTextInputListener, &st);

    // The server takes keyboard focus on this commit, which is what makes the
    // text input usable at all.
    wl_surface* surface = wl_compositor_create_surface(st.compositor);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    assert(st.enters == 1);

    zwp_text_input_v3_enable(st.text_input);
    zwp_text_input_v3_set_surrounding_text(st.text_input, kSurrounding, 5, 5);
    zwp_text_input_v3_set_content_type(st.text_input, ZWP_TEXT_INPUT_V3_CONTENT_HINT_SPELLCHECK,
                                       ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_URL);
    zwp_text_input_v3_set_cursor_rectangle(st.text_input, 1, 2, 3, 4);
    zwp_text_input_v3_commit(st.text_input);
    wl_display_roundtrip(display); // the server applies it and answers
    wl_display_roundtrip(display); // …and we receive the answer

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
    auto text_inputs = luminaria::TextInputManager::create(*display, *seat);
    assert(text_inputs.has_value());

    int new_text_inputs = 0;
    int enables = 0;
    int commits_seen = 0;
    bool enabled_at_commit = false;
    std::string surrounding;
    uint32_t cursor = 0, anchor = 0, hint = 0;
    luminaria::TextInputPurpose purpose = luminaria::TextInputPurpose::normal;
    luminaria::Box cursor_box{};
    uint32_t serial_at_commit = 0;
    bool focused_is_set = false;

    std::vector<luminaria::Signal<luminaria::TextInputEnable>::Connection> enable_conns;
    std::vector<luminaria::Signal<luminaria::TextInputCommit>::Connection> commit_conns;
    auto nti = text_inputs->new_text_input().connect([&](luminaria::NewTextInput& e) {
        ++new_text_inputs;
        luminaria::TextInput* ti = &e.text_input;
        enable_conns.push_back(
            ti->enable.connect([&](luminaria::TextInputEnable&) { ++enables; }));
        commit_conns.push_back(ti->commit.connect([&, ti](luminaria::TextInputCommit&) {
            ++commits_seen;
            enabled_at_commit = ti->enabled();
            surrounding = ti->surrounding_text();
            cursor = ti->surrounding_cursor();
            anchor = ti->surrounding_anchor();
            hint = ti->content_hint();
            purpose = ti->content_purpose();
            cursor_box = ti->cursor_rectangle();
            serial_at_commit = ti->serial();
            focused_is_set = text_inputs->focused() == ti;

            // The input method's answer. None of it reaches the client until
            // send_done().
            ti->send_delete_surrounding_text(2, 0);
            ti->send_preedit_string(kPreedit, 0, 2);
            ti->send_commit_string(kCommitString);
            ti->send_done();
        }));
    });

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

    assert(new_text_inputs == 1);
    assert(enables == 1);
    assert(commits_seen == 1);
    assert(enabled_at_commit);
    assert(surrounding == kSurrounding);
    assert(cursor == 5 && anchor == 5);
    assert(hint == luminaria::text_input_hint_spellcheck);
    assert(purpose == luminaria::TextInputPurpose::url);
    assert(cursor_box.x == 1 && cursor_box.y == 2 && cursor_box.width == 3 &&
           cursor_box.height == 4);
    assert(serial_at_commit == 1); // one commit request received
    assert(focused_is_set);

    assert(g_client.enters == 1);
    assert(g_client.dones == 1);
    assert(g_client.last_serial == 1);
    assert(g_client.delete_before == 2 && g_client.delete_after == 0);
    assert(g_client.preedit == kPreedit);
    assert(g_client.preedit_begin == 0 && g_client.preedit_end == 2);
    assert(g_client.commit_string == kCommitString);
    return 0;
}
