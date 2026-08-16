// Video modes: what an output can be driven at, and what clients are told.
//
// The backend half (`Output::modes()` / `set_mode()`) only does anything real
// on KMS, which needs a free VT — `test_drm` covers that and skips here. What
// this test pins down is the part every backend shares and every client sees:
//
//  - an output that cannot change mode says so, and says yes to the mode it is
//    already in (a caller must not have to compare sizes itself);
//  - `wl_output` advertises the whole list with exactly ONE mode flagged
//    CURRENT, and re-advertises after a switch. A client that is left believing
//    in the old resolution renders at the wrong size.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

import luminaria;
import std;

namespace {

struct SeenMode {
    std::uint32_t flags;
    int width;
    int height;
    int refresh;
};

struct ClientState {
    wl_output* output = nullptr;
    std::vector<SeenMode> modes;
    std::size_t after_first_done = 0;
    int dones = 0;
};

ClientState g_client;

void out_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*,
                  const char*, int32_t) {}
void out_mode(void*, wl_output*, std::uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    g_client.modes.push_back(SeenMode{flags, w, h, refresh});
}
void out_done(void*, wl_output*) {
    if (++g_client.dones == 1) {
        g_client.after_first_done = g_client.modes.size();
    }
}
void out_scale(void*, wl_output*, int32_t) {}
void out_name(void*, wl_output*, const char*) {}
void out_description(void*, wl_output*, const char*) {}
const wl_output_listener kOutputListener{out_geometry, out_mode, out_done,
                                         out_scale,    out_name, out_description};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface,
                     uint32_t version) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "wl_output") == 0) {
        st->output = static_cast<wl_output*>(
            wl_registry_bind(reg, name, &wl_output_interface, version < 4 ? version : 4));
        wl_output_add_listener(st->output, &kOutputListener, st);
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistry, &g_client);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); // the output's own events
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

/// Exactly one CURRENT, and it is the one we asked for.
void check_current(const std::vector<SeenMode>& modes, std::size_t from, std::size_t to, int w,
                   int h) {
    int currents = 0;
    for (std::size_t i = from; i < to; ++i) {
        if ((modes[i].flags & WL_OUTPUT_MODE_CURRENT) != 0) {
            ++currents;
            assert(modes[i].width == w && modes[i].height == h);
        }
    }
    assert(currents == 1);
}

} // namespace

int main() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());

    // --- the backend contract, on a backend with no modes to speak of ---
    luminaria::HeadlessBackend backend(display->event_loop(), /*frame_interval_ms=*/1000);
    luminaria::Output& output = backend.add_output(800, 600);
    assert(output.modes().empty());
    assert(output.current_mode().width == 800 && output.current_mode().height == 600);
    // Asking for the mode it is already in succeeds — the caller should not
    // have to compare sizes before every call.
    assert(output.set_mode(800, 600).has_value());
    assert(!output.set_mode(1024, 768).has_value());
    assert(output.width() == 800 && output.height() == 600);

    // --- what the client is told ---
    auto og = luminaria::OutputGlobal::create(*display, 1920, 1080, "test-0");
    assert(og.has_value());
    og->set_modes({luminaria::OutputMode{1920, 1080, 60000, true},
                   luminaria::OutputMode{1920, 1080, 144000, false},
                   luminaria::OutputMode{1280, 720, 60000, false}});
    og->set_mode(1920, 1080, 60000);

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::size_t before_switch = 0;
    auto switch_mode = display->event_loop().add_timer([&] {
        before_switch = g_client.modes.size();
        assert(before_switch >= 3);
        og->set_mode(1280, 720, 60000);
    });
    switch_mode.arm(150);

    std::thread client_thread(run_client, fds[1]);
    auto stop = display->event_loop().add_timer([&] { display->terminate(); });
    stop.arm(400);
    display->run();
    wl_client_destroy(client);
    client_thread.join();

    // The whole list arrived on bind, with 60Hz current and preferred.
    assert(g_client.after_first_done == 3);
    check_current(g_client.modes, 0, 3, 1920, 1080);
    assert(g_client.modes[0].refresh == 60000);
    assert((g_client.modes[0].flags & WL_OUTPUT_MODE_PREFERRED) != 0);
    assert((g_client.modes[1].flags & WL_OUTPUT_MODE_CURRENT) == 0); // same size, 144Hz

    // And again after the switch, now pointing at 1280x720. Without the
    // re-advertise the client would still believe in 1920x1080.
    assert(g_client.modes.size() == before_switch + 3);
    check_current(g_client.modes, before_switch, g_client.modes.size(), 1280, 720);
    assert(g_client.dones == 2);
    return 0;
}
