// ext-image-copy-capture-v1 cursor sessions: capturing the pointer as a source
// of its own, so a screen recorder can keep it out of the video and composite
// it back at playback time.
//
// This used to be a silent no-op — the request returned without binding the
// new_id the client had just allocated, so the client's very next request went
// to an object libwayland had never heard of and the connection was killed.
// "Unsupported" has to be said in the protocol's own words, not by dropping the
// request on the floor, and the first thing this test checks is that a client
// which uses the session survives using it.
//
// Then the real path: geometry (enter, position, hotspot), the buffer
// constraints for the cursor's own size rather than the output's, and the
// pixels. And finally the same flow with no cursor source registered at all,
// where the session must be created and told `stopped`.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

import luminaria.gpu;
import std;

namespace {

// The cursor the server will report: a 4x3 image, hotspot (1,2), at (100,50).
constexpr int kCurW = 4, kCurH = 3;
constexpr int kHotX = 1, kHotY = 2;
constexpr int kPosX = 100, kPosY = 50;
constexpr std::uint8_t kCurR = 10, kCurG = 200, kCurB = 30;

struct ClientState {
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    ext_output_image_capture_source_manager_v1* source_mgr = nullptr;
    ext_image_copy_capture_manager_v1* capture_mgr = nullptr;
    wl_output* output = nullptr;

    int enters = 0, leaves = 0;
    int pos_x = -1, pos_y = -1;
    int hot_x = -1, hot_y = -1;

    int buffer_w = -1, buffer_h = -1;
    int dones = 0;
    bool stopped = false;
    bool ready = false;
    bool failed = false;
    std::uint8_t got[4] = {0, 0, 0, 0};
    bool finished = false;
};

ClientState g_c;

// ---- cursor session ----
void cs_enter(void*, ext_image_copy_capture_cursor_session_v1*) { ++g_c.enters; }
void cs_leave(void*, ext_image_copy_capture_cursor_session_v1*) { ++g_c.leaves; }
void cs_position(void*, ext_image_copy_capture_cursor_session_v1*, int32_t x, int32_t y) {
    g_c.pos_x = x;
    g_c.pos_y = y;
}
void cs_hotspot(void*, ext_image_copy_capture_cursor_session_v1*, int32_t x, int32_t y) {
    g_c.hot_x = x;
    g_c.hot_y = y;
}
const ext_image_copy_capture_cursor_session_v1_listener kCursorListener{cs_enter, cs_leave,
                                                                       cs_position, cs_hotspot};

// ---- capture session ----
void sess_buffer_size(void*, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
    g_c.buffer_w = static_cast<int>(w);
    g_c.buffer_h = static_cast<int>(h);
}
void sess_shm_format(void*, ext_image_copy_capture_session_v1*, uint32_t) {}
void sess_dmabuf_device(void*, ext_image_copy_capture_session_v1*, wl_array*) {}
void sess_dmabuf_format(void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {}
void sess_done(void*, ext_image_copy_capture_session_v1*) { ++g_c.dones; }
void sess_stopped(void*, ext_image_copy_capture_session_v1*) { g_c.stopped = true; }
const ext_image_copy_capture_session_v1_listener kSessionListener{
    sess_buffer_size, sess_shm_format, sess_dmabuf_device,
    sess_dmabuf_format, sess_done, sess_stopped};

// ---- frame ----
void fr_transform(void*, ext_image_copy_capture_frame_v1*, uint32_t) {}
void fr_damage(void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {}
void fr_presentation_time(void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {}
void fr_ready(void*, ext_image_copy_capture_frame_v1*) { g_c.ready = true; }
void fr_failed(void*, ext_image_copy_capture_frame_v1*, uint32_t) { g_c.failed = true; }
const ext_image_copy_capture_frame_v1_listener kFrameListener{
    fr_transform, fr_damage, fr_presentation_time, fr_ready, fr_failed};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface,
                     uint32_t version) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "wl_shm") == 0) {
        st->shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (std::strcmp(iface, "wl_output") == 0) {
        st->output = static_cast<wl_output*>(
            wl_registry_bind(reg, name, &wl_output_interface, version < 4 ? version : 4));
    } else if (std::strcmp(iface, "wl_seat") == 0) {
        st->seat = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, 1));
    } else if (std::strcmp(iface, "ext_output_image_capture_source_manager_v1") == 0) {
        st->source_mgr = static_cast<ext_output_image_capture_source_manager_v1*>(wl_registry_bind(
            reg, name, &ext_output_image_capture_source_manager_v1_interface, 1));
    } else if (std::strcmp(iface, "ext_image_copy_capture_manager_v1") == 0) {
        st->capture_mgr = static_cast<ext_image_copy_capture_manager_v1*>(
            wl_registry_bind(reg, name, &ext_image_copy_capture_manager_v1_interface, 1));
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
    wl_registry_add_listener(reg, &kRegistry, &g_c);
    wl_display_roundtrip(display);
    assert(g_c.shm != nullptr && g_c.output != nullptr && g_c.source_mgr != nullptr &&
           g_c.capture_mgr != nullptr && g_c.seat != nullptr);
    // The wl_pointer argument is not nullable, so a real one has to exist even
    // though the compositor has a single seat and ignores which it is.
    g_c.pointer = wl_seat_get_pointer(g_c.seat);

    ext_image_capture_source_v1* source =
        ext_output_image_capture_source_manager_v1_create_source(g_c.source_mgr, g_c.output);
    ext_image_copy_capture_cursor_session_v1* cursor =
        ext_image_copy_capture_manager_v1_create_pointer_cursor_session(g_c.capture_mgr, source,
                                                                        g_c.pointer);
    ext_image_copy_capture_cursor_session_v1_add_listener(cursor, &kCursorListener, nullptr);
    // The request that used to be a no-op. If the id was never bound, THIS is
    // where libwayland disconnects us with "invalid object".
    ext_image_copy_capture_session_v1* session =
        ext_image_copy_capture_cursor_session_v1_get_capture_session(cursor);
    ext_image_copy_capture_session_v1_add_listener(session, &kSessionListener, nullptr);
    if (wl_display_roundtrip(display) < 0) {
        wl_display_disconnect(display);
        return; // g_c.finished stays false: the server side will notice
    }

    if (!g_c.stopped && g_c.buffer_w > 0) {
        // Allocate to the constraints the CURSOR advertised, not the output's.
        const int stride = g_c.buffer_w * 4;
        const int size = stride * g_c.buffer_h;
        int bfd = memfd_create("curcap", MFD_CLOEXEC);
        assert(ftruncate(bfd, size) == 0);
        auto* px = static_cast<std::uint8_t*>(
            mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0));
        wl_shm_pool* pool = wl_shm_create_pool(g_c.shm, bfd, size);
        wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, g_c.buffer_w, g_c.buffer_h, stride,
                                                      WL_SHM_FORMAT_ARGB8888);
        ext_image_copy_capture_frame_v1* frame =
            ext_image_copy_capture_session_v1_create_frame(session);
        ext_image_copy_capture_frame_v1_add_listener(frame, &kFrameListener, nullptr);
        ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
        ext_image_copy_capture_frame_v1_capture(frame);
        wl_display_roundtrip(display);
        std::memcpy(g_c.got, px, 4);
        munmap(px, size);
        wl_shm_pool_destroy(pool);
        close(bfd);
    }
    g_c.finished = true;
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
}

struct DestroyCtx {
    wl_listener listener;
    luminaria::Display* display;
    bool gone = false;
};
void on_client_destroy(wl_listener* l, void*) {
    auto* ctx =
        reinterpret_cast<DestroyCtx*>(reinterpret_cast<char*>(l) - offsetof(DestroyCtx, listener));
    ctx->gone = true;
    ctx->display->terminate();
}

/// One run of the whole client flow. `with_cursor` decides whether the server
/// has a cursor source at all.
void run(bool with_cursor) {
    g_c = ClientState{};

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    assert(display->init_shm().has_value());
    auto og = luminaria::OutputGlobal::create(*display, 80, 60, "cap-0");
    assert(og.has_value());
    auto screencopy = luminaria::ScreencopyManager::create(*display);
    assert(screencopy.has_value());
    // Only so the client has a wl_pointer to name; the request's pointer
    // argument is not nullable.
    auto seat = luminaria::Seat::create(*display);
    assert(seat.has_value());

    og->on_bind([&](wl_resource* res) {
        screencopy->add_output(res, 80, 60,
                               [](int, int, int w, int h, std::vector<std::uint8_t>& rgba) {
                                   rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
                                   return true;
                               });
    });

    if (with_cursor) {
        screencopy->set_cursor_source([](wl_resource*, luminaria::CursorCapture& out) {
            out.width = kCurW;
            out.height = kCurH;
            out.hotspot_x = kHotX;
            out.hotspot_y = kHotY;
            out.x = kPosX;
            out.y = kPosY;
            out.rgba.resize(static_cast<std::size_t>(kCurW) * kCurH * 4);
            for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
                out.rgba[i + 0] = kCurR;
                out.rgba[i + 1] = kCurG;
                out.rgba[i + 2] = kCurB;
                out.rgba[i + 3] = 255;
            }
            return true;
        });
    }

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display, false};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_client, fds[1]);
    auto stop = display->event_loop().add_timer([&] { display->terminate(); });
    stop.arm(2000);
    display->run();
    // The client may have dropped its own connection already; destroying it
    // twice is a libwayland re-entrancy warning, not a second cleanup.
    if (!destroy_ctx.gone) {
        wl_client_destroy(client);
    }
    client_thread.join();

    // Whatever else happened, the client got all the way through without being
    // disconnected. A no-op create_pointer_cursor_session fails right here.
    assert(g_c.finished);

    if (with_cursor) {
        assert(!g_c.stopped);
        assert(g_c.enters == 1 && g_c.leaves == 0);
        assert(g_c.pos_x == kPosX && g_c.pos_y == kPosY);
        assert(g_c.hot_x == kHotX && g_c.hot_y == kHotY);
        // The cursor's own size, not the 80x60 output's — a client that
        // allocated to the output would waste most of it and place it wrong.
        assert(g_c.buffer_w == kCurW && g_c.buffer_h == kCurH);
        assert(g_c.dones == 1);
        assert(g_c.ready && !g_c.failed);
        // ARGB8888 little-endian: B, G, R, A.
        assert(g_c.got[0] == kCurB && g_c.got[1] == kCurG && g_c.got[2] == kCurR);
        assert(g_c.got[3] == 255);
    } else {
        // No cursor to capture: the session exists and says so, rather than
        // leaving the client waiting for constraints that never arrive.
        assert(g_c.stopped);
        assert(g_c.enters == 0);
        assert(g_c.buffer_w == -1);
        assert(!g_c.ready);
    }
}

} // namespace

int main() {
    run(/*with_cursor=*/true);
    run(/*with_cursor=*/false);
    return 0;
}
