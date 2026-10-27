// SceneRenderer answers the display by itself, and Cursor keeps the pointer
// on every output: the two contracts every compositor used to write by hand.
//
// A client draws, asks for a frame callback and presentation feedback, and
// waits for both. The server does nothing but `present()` its scene list — no
// present handler, no frame-callback loop, no `unchanged` branch — so a client
// that finishes every round proves all three are the library's now:
//
//   1. a new buffer: composited, presented, callback + feedback;
//   2. commits the screen cannot show — the server has covered the window
//      with an opaque rectangle — so the frame is `unchanged` on the GPU, no
//      present follows it, and the callback must still come (a visible shm
//      commit is always repainted, so covering it is how a test reaches
//      `unchanged`; a dmabuf client gets there by committing without damage);
//   3. a new buffer again: on the CPU path nothing but the library's own
//      commit watch can wake the output for it.
//
// And idling: with nothing committing, a software output must stop asking for
// frames as the GPU one does, without going deaf — a new buffer, or a commit
// that changes nothing but wants its callback, must still be answered.
//
// Run twice, on the machine's GPU when there is one and then with Vulkan made
// unavailable, so the CPU path is covered on every machine.
//
// Then the cursor, server-side: where the sprite lands on each output, that a
// moved pointer asks a composited output for a frame, and what screen capture
// is told.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "presentation-time-client-protocol.h"

import luminaria.shell;
import std;

namespace {

constexpr int kOutW = 64, kOutH = 48;
constexpr int kWinW = 16, kWinH = 16;
constexpr int kRounds = 5; // new buffer, three empty commits, new buffer
constexpr bool content_round(int round) { return round == 0 || round == kRounds - 1; }

std::atomic<int> g_rounds{0};
std::atomic<int> g_presented{0};
std::atomic<int> g_frames{0}; // `frame` events the idle session's output emitted
constexpr int kIdleMs = 100;

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wp_presentation* presentation = nullptr;
};

wl_buffer* make_buffer(ClientState& st, std::uint32_t argb) {
    const int stride = kWinW * 4;
    const int size = stride * kWinH;
    const int fd = memfd_create("luminaria-test", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, size) == 0);
    auto* px = static_cast<std::uint32_t*>(
        mmap(nullptr, static_cast<std::size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(px != MAP_FAILED);
    for (int i = 0; i < kWinW * kWinH; ++i) {
        px[i] = argb;
    }
    munmap(px, static_cast<std::size_t>(size));
    wl_shm_pool* pool = wl_shm_create_pool(st.shm, fd, size);
    wl_buffer* buffer =
        wl_shm_pool_create_buffer(pool, 0, kWinW, kWinH, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t) {
    auto& st = *static_cast<ClientState*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        st.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "wl_shm") == 0) {
        st.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, "wp_presentation") == 0) {
        st.presentation = static_cast<wp_presentation*>(
            wl_registry_bind(registry, name, &wp_presentation_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

void frame_done(void* data, wl_callback* cb, uint32_t) {
    *static_cast<bool*>(data) = true;
    wl_callback_destroy(cb);
}
const wl_callback_listener kFrameListener{frame_done};

void fb_sync_output(void*, struct wp_presentation_feedback*, wl_output*) {}
void fb_presented(void*, struct wp_presentation_feedback* fb, uint32_t, uint32_t, uint32_t, uint32_t,
                  uint32_t, uint32_t, uint32_t) {
    g_presented.fetch_add(1);
    wp_presentation_feedback_destroy(fb);
}
void fb_discarded(void*, struct wp_presentation_feedback* fb) { wp_presentation_feedback_destroy(fb); }
const wp_presentation_feedback_listener kFeedbackListener{fb_sync_output, fb_presented,
                                                          fb_discarded};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    assert(display != nullptr);
    ClientState st;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display);
    assert(st.compositor != nullptr && st.shm != nullptr && st.presentation != nullptr);

    wl_surface* surface = wl_compositor_create_surface(st.compositor);
    for (int round = 0; round < kRounds; ++round) {
        bool done = false;
        wl_callback* cb = wl_surface_frame(surface);
        wl_callback_add_listener(cb, &kFrameListener, &done);
        if (content_round(round)) {
            struct wp_presentation_feedback* fb = wp_presentation_feedback(st.presentation, surface);
            wp_presentation_feedback_add_listener(fb, &kFeedbackListener, nullptr);
            wl_surface_attach(surface, make_buffer(st, 0xFF000000u | (round + 1) * 0x40), 0, 0);
            wl_surface_damage(surface, 0, 0, kWinW, kWinH);
        }
        wl_surface_commit(surface);
        wl_display_flush(display);
        // A compositor that forgot any of the three hangs here, and the
        // server's timeout ends the test with too few rounds.
        while (!done) {
            if (wl_display_dispatch(display) < 0) {
                wl_display_disconnect(display);
                return;
            }
        }
        g_rounds.fetch_add(1);
    }
    wl_display_roundtrip(display); // the last feedback, if it is still in flight
    wl_display_disconnect(display);
}

/// Commit `argb` (or nothing, when 0) and wait for the frame callback.
bool commit_and_wait(wl_display* display, ClientState& st, wl_surface* surface,
                     std::uint32_t argb) {
    bool done = false;
    wl_callback* cb = wl_surface_frame(surface);
    wl_callback_add_listener(cb, &kFrameListener, &done);
    if (argb != 0) {
        wl_surface_attach(surface, make_buffer(st, argb), 0, 0);
        wl_surface_damage(surface, 0, 0, kWinW, kWinH);
    }
    wl_surface_commit(surface);
    wl_display_flush(display);
    while (!done) {
        if (wl_display_dispatch(display) < 0) {
            return false;
        }
    }
    return true;
}

std::atomic<int> g_idle_frames{-1};   // frames emitted while the client sat still
std::atomic<int> g_idle_answered{0};  // commits that got their callback

void run_idle_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    assert(display != nullptr);
    ClientState st;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &st);
    wl_display_roundtrip(display);
    wl_surface* surface = wl_compositor_create_surface(st.compositor);

    do {
        if (!commit_and_wait(display, st, surface, 0xFFFF0000u)) { // red
            break;
        }
        g_idle_answered.fetch_add(1);
        // Let the frame that follows the first present drain, then sit still.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const int before = g_frames.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs));
        g_idle_frames = g_frames.load() - before;
        // A commit that changes nothing on screen still wants its callback.
        if (!commit_and_wait(display, st, surface, 0)) {
            break;
        }
        g_idle_answered.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs));
        // And a new buffer must still be drawn.
        if (!commit_and_wait(display, st, surface, 0xFF00FF00u)) { // green
            break;
        }
        g_idle_answered.fetch_add(1);
    } while (false);
    wl_display_disconnect(display);
}

/// An output paced the way a display is: `present` only for a frame that was
/// actually committed — a page flip — and never for one that was not. Headless
/// reports a presentation before every frame, which would release the frame
/// callbacks of an `unchanged` frame by accident and hide the bug this test is
/// for.
class FlipOutput final : public luminaria::Output {
public:
    explicit FlipOutput(luminaria::EventLoop loop) : Output(kOutW, kOutH) {
        timer_ = loop.add_timer([this] {
            if (std::exchange(flip_pending_, false)) {
                luminaria::PresentEvent shown{*this};
                const auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
                shown.tv_sec = static_cast<std::uint64_t>(ns / 1'000'000'000);
                shown.tv_nsec = static_cast<std::uint32_t>(ns % 1'000'000'000);
                present.emit(shown);
            }
            emit_frame();
        });
    }
    luminaria::Status commit(luminaria::Color) override { return flip(); }
    luminaria::Status commit_frame(std::span<const luminaria::Pixel>, int, int) override {
        return flip();
    }

protected:
    void arm_frame() override { timer_.arm(1); }

private:
    luminaria::Status flip() {
        flip_pending_ = true;
        return luminaria::ok();
    }
    luminaria::EventSource timer_;
    bool flip_pending_ = false;
};

struct DestroyCtx {
    wl_listener listener;
    luminaria::Display* display;
};
void on_client_destroy(wl_listener* l, void*) {
    auto* ctx = reinterpret_cast<DestroyCtx*>(reinterpret_cast<char*>(l) -
                                              offsetof(DestroyCtx, listener));
    ctx->display->terminate();
}

/// One whole client session against `renderer`. Returns whether the output
/// was on the GPU path.
bool run_session(luminaria::SceneRenderer& renderer) {
    g_rounds = 0;
    g_presented = 0;
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value() && display->init_shm().has_value());
    auto compositor = luminaria::Compositor::create(*display);
    auto presentation = luminaria::Presentation::create(*display);
    assert(compositor.has_value() && presentation.has_value());
    renderer.set_presentation(&*presentation);

    FlipOutput output(display->event_loop());
    luminaria::SceneOutput state;
    (void)renderer.reset(state, output, DRM_FORMAT_XRGB8888); // CPU on failure
    const bool gpu = state.gpu_output();

    luminaria::SurfaceId window;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        window = e.surface.id();
        output.schedule_frame(); // a surface nothing has drawn yet: ours to wake
    });
    std::vector<luminaria::SceneItem> scene;
    int unchanged = 0;
    auto on_frame = output.frame.connect([&](luminaria::FrameEvent&) {
        scene.clear();
        if (luminaria::surface_from_id(window) != nullptr) {
            scene.push_back({.box = {0, 0, kWinW, kWinH}, .surface = window});
            // Hidden for the empty rounds, uncovered for the last one.
            if (!content_round(g_rounds.load())) {
                scene.push_back({.kind = luminaria::SceneItem::Kind::rect,
                                 .box = {0, 0, kWinW, kWinH},
                                 .color = {0, 1, 0, 1}});
            }
        }
        const auto shown = renderer.present(state, output, scene, luminaria::Color{0, 0, 1, 1});
        assert(shown.has_value());
        unchanged += *shown == luminaria::SceneOutcome::unchanged;
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_client, fds[1]);
    // Cutting the client off is what unblocks its thread when a round hangs.
    auto timeout = display->event_loop().add_timer([&] { wl_client_destroy(client); });
    timeout.arm(5000);
    display->run();
    client_thread.join();
    renderer.set_presentation(nullptr);

    std::fprintf(stderr, "%s: %d/%d rounds, %d presented, %d unchanged frames\n",
                 gpu ? "gpu" : "cpu", g_rounds.load(), kRounds, g_presented.load(), unchanged);
    assert(g_rounds.load() == kRounds);
    assert(g_presented.load() == 2); // only the content rounds asked for feedback
    assert(!gpu || unchanged > 0);   // or round 2 tested nothing
    return gpu;
}

/// The idle half: the same server as `run_session`, a client that stops
/// committing for `kIdleMs`, and a count of the frames the output asked for
/// meanwhile. A free-running software output emits ~100 of them.
void run_idle_session(luminaria::SceneRenderer& renderer) {
    g_frames = 0;
    g_idle_frames = -1;
    g_idle_answered = 0;
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value() && display->init_shm().has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());

    FlipOutput output(display->event_loop());
    luminaria::SceneOutput state;
    (void)renderer.reset(state, output, DRM_FORMAT_XRGB8888);
    const bool gpu = state.gpu_output();

    luminaria::SurfaceId window;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        window = e.surface.id();
        output.schedule_frame();
    });
    std::vector<luminaria::SceneItem> scene;
    int repainted = 0, unchanged = 0;
    auto on_frame = output.frame.connect([&](luminaria::FrameEvent&) {
        g_frames.fetch_add(1);
        scene.clear();
        if (luminaria::surface_from_id(window) != nullptr) {
            scene.push_back({.box = {0, 0, kWinW, kWinH}, .surface = window});
        }
        const auto shown = renderer.present(state, output, scene, luminaria::Color{0, 0, 1, 1});
        assert(shown.has_value());
        unchanged += *shown == luminaria::SceneOutcome::unchanged;
        repainted += *shown != luminaria::SceneOutcome::unchanged;
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_idle_client, fds[1]);
    auto timeout = display->event_loop().add_timer([&] { wl_client_destroy(client); });
    timeout.arm(5000);
    display->run();
    client_thread.join();

    const auto pixels = renderer.read_back(state);
    assert(pixels.has_value());
    const luminaria::Pixel last = (*pixels)[0];

    std::fprintf(stderr, "%s idle: %d frames in %d ms, %d repainted, %d unchanged, %d answered\n",
                 gpu ? "gpu" : "cpu", g_idle_frames.load(), kIdleMs, repainted, unchanged,
                 g_idle_answered.load());
    assert(g_idle_answered.load() == 3); // none of the three commits was left waiting
    assert(g_idle_frames.load() >= 0 && g_idle_frames.load() <= 4);
    assert(unchanged > 0);
    // The last commit was green, and it was drawn.
    assert(last.g == 0xFF && last.r == 0 && last.b == 0);
}

void cursor_follows_the_pointer(luminaria::SceneRenderer& renderer) {
    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto seat = luminaria::Seat::create(*display);
    assert(seat.has_value());
    luminaria::HeadlessOutput left(display->event_loop(), kOutW, kOutH, 1);
    luminaria::HeadlessOutput right(display->event_loop(), kOutW, kOutH, 1);
    luminaria::OutputLayout layout;
    layout.add(left, 0, 0);
    layout.add(right, kOutW, 0);
    luminaria::SceneOutput left_state, right_state;
    (void)renderer.reset(left_state, left, DRM_FORMAT_XRGB8888);
    (void)renderer.reset(right_state, right, DRM_FORMAT_XRGB8888);

    luminaria::Cursor cursor(renderer, layout, *seat);
    if (cursor.theme() == nullptr) {
        // No cursor theme on this machine: nothing to draw, nothing to place.
        std::fprintf(stderr, "cursor: no theme installed, placement not checked\n");
        return;
    }

    int left_frames = 0;
    auto counted = left.frame.connect([&](luminaria::FrameEvent&) { ++left_frames; });

    // On the left output, at the pointer minus the hotspot.
    assert(cursor.move_to(10, 20));
    const luminaria::SceneCursor here = cursor.on(left);
    assert(here.image != nullptr);
    assert(here.x == 10 - here.image->hotspot_x && here.y == 20 - here.image->hotspot_y);
    // Nowhere near the right one, so its plane (were there one) comes down.
    assert(cursor.on(right).image == nullptr);

    // Across the seam: the right output sees it in its own coordinates.
    assert(cursor.move_to(kOutW + 30, 5));
    const luminaria::SceneCursor there = cursor.on(right);
    assert(there.image != nullptr && there.x == 30 - there.image->hotspot_x);

    // Screen capture gets the hotspot in the output's pixels, and only from
    // the output the pointer is on.
    luminaria::CursorCapture captured;
    assert(cursor.capture(right, captured));
    assert(captured.x == 30 && captured.y == 5 && !captured.rgba.empty());
    assert(!cursor.capture(left, captured));

    // A headless output has no cursor plane, so moving the pointer is a
    // repaint there: the move must have asked for a frame.
    auto stop = display->event_loop().add_timer([&] { display->terminate(); });
    stop.arm(50);
    display->run();
    assert(left_frames > 0);

    cursor.leave();
    assert(cursor.on(left).image == nullptr && cursor.on(right).image == nullptr);
    std::fprintf(stderr, "cursor: placed, localized, captured\n");
}

// Nothing a device said yet: the first output to arrive finds the pointer in its
// middle, absolute motion lands on the output the event names (the primary one
// when it names none), and a pointer that left the window stays gone.
void cursor_places_itself(luminaria::SceneRenderer& renderer) {
    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto seat = luminaria::Seat::create(*display);
    assert(seat.has_value());
    luminaria::HeadlessOutput left(display->event_loop(), kOutW, kOutH, 1);
    luminaria::HeadlessOutput right(display->event_loop(), kOutW, kOutH, 1);
    luminaria::OutputLayout layout;
    luminaria::Cursor cursor(renderer, layout, *seat);
    assert(!cursor.pointer().present());

    layout.add(left, 0, 0);
    assert(cursor.pointer().present());
    assert(cursor.pointer().x() == kOutW / 2.0 && cursor.pointer().y() == kOutH / 2.0);

    layout.add(right, kOutW, 0);
    assert(cursor.pointer().x() == kOutW / 2.0); // the second arrival does not move it

    cursor.move_to(luminaria::PointerMotionAbsEvent{10, 12, nullptr});
    assert(cursor.pointer().x() == 10 && cursor.pointer().y() == 12);
    cursor.move_to(luminaria::PointerMotionAbsEvent{5, 6, &right});
    assert(cursor.pointer().x() == kOutW + 5 && cursor.pointer().y() == 6);

    cursor.leave();
    layout.add(right, kOutW, 0);
    assert(!cursor.pointer().present());
    std::fprintf(stderr, "cursor: centered, absolute motion mapped\n");
}

} // namespace

int main() {
    {
        luminaria::SceneRenderer renderer;
        (void)run_session(renderer);
        run_idle_session(renderer);
        cursor_follows_the_pointer(renderer);
        cursor_places_itself(renderer);
    }
    // The same on the CPU path, wherever this runs: a loader with no driver
    // to load cannot create an instance, which is exactly a GPU-less machine.
    setenv("VK_ICD_FILENAMES", "/nonexistent/luminaria-test-icd.json", 1);
    setenv("VK_DRIVER_FILES", "/nonexistent/luminaria-test-icd.json", 1);
    luminaria::SceneRenderer cpu;
    assert(!cpu.gpu_enabled());
    assert(!run_session(cpu));
    run_idle_session(cpu);
    cursor_follows_the_pointer(cpu);
    cursor_places_itself(cpu);
    return 0;
}
