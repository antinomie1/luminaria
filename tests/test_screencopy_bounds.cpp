// screencopy writes INTO a client's buffer, so a layout it fails to validate is
// memory corruption in someone else's address space, not just a bad read.
//
// Two ways a client can lie, both accepted upstream:
//   * a short stride — libwayland validates `stride >= width`, bytes against
//     pixels, so stride == width passes for a 4-byte format;
//   * a buffer smaller than the capture — ext-image-copy-capture's
//     `attach_buffer` takes any wl_buffer at all, and the size being captured
//     is the output's, not one the client chose.
//
// Rather than rely on a fault, each pool is allocated with a canary region past
// the declared buffer and checked byte for byte afterwards: an overrun is then
// observable even when it happens to land on mapped memory, which is the case
// that silently corrupts instead of crashing.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-server-core.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

import luminaria.gpu;
import std;

namespace {

constexpr int kOutW = 80;
constexpr int kOutH = 60;
constexpr std::uint8_t kCanary = 0xAB;
constexpr int kCanaryBytes = 64 * 1024;

enum class Mode {
    ShortStride,  ///< stride == width: a quarter of a real ARGB8888 row
    UndersizedBuffer, ///< correctly strided, but far smaller than the capture
    Correct,      ///< what a well-behaved client sends
};

struct ClientState {
    wl_shm* shm = nullptr;
    wl_output* output = nullptr;
    ext_output_image_capture_source_manager_v1* source_mgr = nullptr;
    ext_image_copy_capture_manager_v1* capture_mgr = nullptr;

    Mode mode = Mode::Correct;
    int buffer_w = 0;
    int buffer_h = 0;
    bool stopped = false;
    bool ready = false;
    bool failed = false;
    bool canary_intact = true;
    bool reached_capture = false;
};
ClientState g_c;

void sess_buffer_size(void*, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
    g_c.buffer_w = static_cast<int>(w);
    g_c.buffer_h = static_cast<int>(h);
}
void sess_shm_format(void*, ext_image_copy_capture_session_v1*, uint32_t) {}
void sess_dmabuf_device(void*, ext_image_copy_capture_session_v1*, wl_array*) {}
void sess_dmabuf_format(void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {}
void sess_done(void*, ext_image_copy_capture_session_v1*) {}
void sess_stopped(void*, ext_image_copy_capture_session_v1*) { g_c.stopped = true; }
const ext_image_copy_capture_session_v1_listener kSessionListener{
    sess_buffer_size, sess_shm_format, sess_dmabuf_device, sess_dmabuf_format,
    sess_done,        sess_stopped};

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

/// Geometry the client declares for this run. Only `Correct` describes the
/// memory it actually backed the buffer with.
void declared_geometry(Mode mode, int& w, int& h, int& stride) {
    switch (mode) {
    case Mode::ShortStride:
        w = g_c.buffer_w;
        h = g_c.buffer_h;
        stride = w; // not w * 4
        break;
    case Mode::UndersizedBuffer:
        w = 8;
        h = 6;
        stride = w * 4;
        break;
    case Mode::Correct:
        w = g_c.buffer_w;
        h = g_c.buffer_h;
        stride = w * 4;
        break;
    }
}

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistry, &g_c);
    wl_display_roundtrip(display);
    assert(g_c.shm != nullptr && g_c.output != nullptr && g_c.source_mgr != nullptr &&
           g_c.capture_mgr != nullptr);

    ext_image_capture_source_v1* source =
        ext_output_image_capture_source_manager_v1_create_source(g_c.source_mgr, g_c.output);
    ext_image_copy_capture_session_v1* session =
        ext_image_copy_capture_manager_v1_create_session(g_c.capture_mgr, source, 0);
    ext_image_copy_capture_session_v1_add_listener(session, &kSessionListener, nullptr);
    if (wl_display_roundtrip(display) < 0) {
        wl_display_disconnect(display);
        return;
    }

    if (!g_c.stopped && g_c.buffer_w > 0) {
        int w = 0;
        int h = 0;
        int stride = 0;
        declared_geometry(g_c.mode, w, h, stride);

        const int declared = stride * h;
        const int size = declared + kCanaryBytes;
        const int bfd = memfd_create("copybounds", MFD_CLOEXEC);
        assert(bfd >= 0);
        assert(ftruncate(bfd, size) == 0);
        auto* px = static_cast<std::uint8_t*>(
            mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0));
        assert(px != MAP_FAILED);
        std::memset(px, kCanary, static_cast<std::size_t>(size));

        wl_shm_pool* pool = wl_shm_create_pool(g_c.shm, bfd, size);
        wl_buffer* buffer =
            wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
        ext_image_copy_capture_frame_v1* frame =
            ext_image_copy_capture_session_v1_create_frame(session);
        ext_image_copy_capture_frame_v1_add_listener(frame, &kFrameListener, nullptr);
        ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
        ext_image_copy_capture_frame_v1_capture(frame);
        g_c.reached_capture = true;
        wl_display_roundtrip(display);

        // Anything past what the client declared must be untouched.
        for (int i = declared; i < size; ++i) {
            if (px[i] != kCanary) {
                g_c.canary_intact = false;
                break;
            }
        }
        munmap(px, static_cast<std::size_t>(size));
        wl_shm_pool_destroy(pool);
        close(bfd);
    }
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

void run(Mode mode) {
    g_c = ClientState{};
    g_c.mode = mode;

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    assert(display->init_shm().has_value());
    auto og = luminaria::OutputGlobal::create(*display, kOutW, kOutH, "cap-0");
    assert(og.has_value());
    auto screencopy = luminaria::ScreencopyManager::create(*display);
    assert(screencopy.has_value());

    og->on_bind([&](wl_resource* res) {
        screencopy->add_output(res, kOutW, kOutH,
                               [](int, int, int w, int h, std::vector<std::uint8_t>& rgba) {
                                   rgba.assign(static_cast<std::size_t>(w) * h * 4, 0x7F);
                                   return true;
                               });
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx ctx{{}, &*display};
    ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &ctx.listener);

    std::thread client_thread(run_client, fds[1]);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(5000);
    display->run();
    client_thread.join();
}

} // namespace

int main() {
    // A short stride must be refused, and must not have written past the
    // buffer the client declared.
    run(Mode::ShortStride);
    assert(g_c.reached_capture);
    assert(g_c.canary_intact);
    assert(g_c.failed);
    assert(!g_c.ready);

    // Same for a buffer far smaller than the capture: attach_buffer accepts any
    // wl_buffer, so this is caught at the write or not at all.
    run(Mode::UndersizedBuffer);
    assert(g_c.reached_capture);
    assert(g_c.canary_intact);
    assert(g_c.failed);
    assert(!g_c.ready);

    // A correct buffer must still capture — the fix cannot be "refuse all".
    run(Mode::Correct);
    assert(g_c.reached_capture);
    assert(g_c.canary_intact);
    assert(g_c.ready);
    assert(!g_c.failed);
    return 0;
}
