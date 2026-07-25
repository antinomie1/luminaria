// Damage tracking and frame pacing, end to end with a real client.
//
// The client attaches a buffer, reports damage on part of it, and asks for both
// a wl_surface.frame callback and wp_presentation feedback. The server must:
//   - see exactly the damage rect the client reported (clipped to the surface),
//   - NOT fire the frame callback at commit time,
//   - fire it, and the presentation feedback, only when told the frame is up.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "presentation-time-client-protocol.h"

import luminaria;

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wp_presentation* presentation = nullptr;
    bool frame_done = false;
    bool presented = false;
    std::uint32_t presented_nsec = 0;
};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "wl_compositor") == 0) {
        st->compositor =
            static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(iface, "wl_shm") == 0) {
        st->shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (std::strcmp(iface, "wp_presentation") == 0) {
        st->presentation =
            static_cast<wp_presentation*>(wl_registry_bind(reg, name, &wp_presentation_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

void on_frame(void* data, wl_callback* cb, uint32_t) {
    static_cast<ClientState*>(data)->frame_done = true;
    wl_callback_destroy(cb);
}
const wl_callback_listener kFrame{on_frame};

// The interface struct and the request share a name; `struct` disambiguates.
void fb_sync_output(void*, struct wp_presentation_feedback*, wl_output*) {}
void fb_presented(void* data, struct wp_presentation_feedback*, uint32_t, uint32_t, uint32_t tv_nsec,
                  uint32_t, uint32_t, uint32_t, uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    st->presented = true;
    st->presented_nsec = tv_nsec;
}
void fb_discarded(void*, struct wp_presentation_feedback*) {}
const wp_presentation_feedback_listener kFeedback{fb_sync_output, fb_presented, fb_discarded};

// The client blocks here until the server says the frame is up, so both the
// "not yet" and the "now" halves of the pacing contract are observed.
void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState st;
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistry, &st);
    wl_display_roundtrip(display);
    assert(st.compositor != nullptr && st.shm != nullptr && st.presentation != nullptr);

    const int w = 16, h = 16, stride = w * 4, size = stride * h;
    int bfd = memfd_create("cbuf", MFD_CLOEXEC);
    assert(ftruncate(bfd, size) == 0);
    wl_shm_pool* pool = wl_shm_create_pool(st.shm, bfd, size);
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_surface* surface = wl_compositor_create_surface(st.compositor);

    struct wp_presentation_feedback* feedback =
        wp_presentation_feedback(st.presentation, surface);
    wp_presentation_feedback_add_listener(feedback, &kFeedback, &st);
    wl_callback* frame = wl_surface_frame(surface);
    wl_callback_add_listener(frame, &kFrame, &st);

    // First commit: a brand-new buffer damages all of itself, whatever the
    // client says.
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    // Commit alone must not release the client to draw again.
    assert(!st.frame_done);
    assert(!st.presented);

    // Now wait for the server's presentation.
    while (!st.frame_done || !st.presented) {
        if (wl_display_dispatch(display) < 0) {
            break;
        }
    }
    assert(st.presented_nsec == 456);

    // Second commit: same buffer, only a corner redrawn.
    wl_surface_damage(surface, 2, 3, 4, 5);
    wl_surface_damage(surface, 100, 100, 8, 8); // outside the buffer: must be dropped
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    wl_shm_pool_destroy(pool);
    close(bfd);
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

std::vector<std::vector<luminaria::Box>> g_damage; // one entry per commit

} // namespace

int main() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    assert(display->init_shm().has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto presentation = luminaria::Presentation::create(*display);
    assert(presentation.has_value());
    // An unstarted headless output: we only need something to name as the
    // display that showed the frame, and we drive the timing by hand.
    luminaria::HeadlessBackend backend{display->event_loop()};
    luminaria::Output& output = backend.add_output(64, 64);

    std::vector<luminaria::Signal<luminaria::SurfaceCommit>::Connection> conns;
    luminaria::EventSource present_timer;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        luminaria::Surface* s = &e.surface;
        conns.push_back(e.surface.commit.connect([&, s](luminaria::SurfaceCommit&) {
            g_damage.push_back(s->damage());
            // Pretend the frame reached the screen one tick later, the way a
            // vblank would arrive.
            present_timer = display->event_loop().add_timer([&, s] {
                luminaria::PresentEvent event{output, 12, 456, 16666666, 7, true, true};
                s->send_frame_done(event.time_ms());
                presentation->notify_presented(*s, event);
                s->clear_damage();
            });
            present_timer.arm(1);
        }));
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

    assert(g_damage.size() == 2);
    // A first buffer of a new size is damaged in full.
    assert(g_damage[0].size() == 1);
    assert((g_damage[0][0] == luminaria::Box{0, 0, 16, 16}));
    // Then only what the client reported, and only the part inside the buffer.
    assert(g_damage[1].size() == 1);
    assert((g_damage[1][0] == luminaria::Box{2, 3, 4, 5}));
    return 0;
}
