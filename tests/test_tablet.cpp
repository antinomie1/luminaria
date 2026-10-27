// zwp_tablet_manager_v2, end to end through `TabletManager::feed()`: the same
// InputEvents a LibinputBackend emits go in, and a real tablet-v2 client on the
// other end of a socketpair must see a coherent pen.
//
//   * a client that binds sees the tablet, the tool (with its capabilities) and
//     the pad (with its group, ring and strip) announced to it;
//   * a pen over the window sends proximity_in with the tablet and surface, then
//     the whole initial state — position in SURFACE coordinates, pressure,
//     distance, tilt — closed by a frame;
//   * only the axes the pen has are sent; hover, tip down, barrel button and tip
//     up come as the protocol orders them;
//   * sliding the pen off the window sends proximity_out, sliding back sends a
//     fresh proximity_in, and the pad enters and leaves with it;
//   * pad buttons announce a mode switch before the button, rings and strips
//     carry their source and position;
//   * the client's set_cursor, answered with the proximity serial, reaches the
//     compositor — and one with a stale serial does not;
//   * unplugging the tablet sends `removed`.
#include <cassert>
#include <cstdarg>
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

#include "tablet-v2-client-protocol.h"
#include "xdg-shell-client-protocol.h"

import luminaria;
import std;

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    xdg_wm_base* wm_base = nullptr;
    zwp_tablet_manager_v2* manager = nullptr;
    wl_surface* surface = nullptr;
    wl_surface* cursor = nullptr;
    bool configured = false;

    std::vector<std::string> tablet_log, tool_log, pad_log;
    std::uint32_t proximity_serial = 0;
    bool set_cursor_on_enter = false;
};
ClientState g_c;

__attribute__((format(printf, 2, 3))) void log(std::vector<std::string>& to, const char* fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    to.emplace_back(buf);
}

// ---- tablet
void t_name(void*, zwp_tablet_v2*, const char* n) { log(g_c.tablet_log, "name %s", n); }
void t_id(void*, zwp_tablet_v2*, uint32_t v, uint32_t p) { log(g_c.tablet_log, "id %04x:%04x", v, p); }
void t_path(void*, zwp_tablet_v2*, const char* p) { log(g_c.tablet_log, "path %s", p); }
void t_done(void*, zwp_tablet_v2*) { log(g_c.tablet_log, "done"); }
void t_removed(void*, zwp_tablet_v2*) { log(g_c.tablet_log, "removed"); }
void t_bustype(void*, zwp_tablet_v2*, uint32_t) {}
const zwp_tablet_v2_listener kTablet{t_name, t_id, t_path, t_done, t_removed, t_bustype};

// ---- tool
void tool_type(void*, zwp_tablet_tool_v2*, uint32_t t) { log(g_c.tool_log, "type %x", t); }
void tool_serial(void*, zwp_tablet_tool_v2*, uint32_t hi, uint32_t lo) {
    log(g_c.tool_log, "serial %x:%x", hi, lo);
}
void tool_wacom(void*, zwp_tablet_tool_v2*, uint32_t, uint32_t) {}
void tool_cap(void*, zwp_tablet_tool_v2*, uint32_t c) { log(g_c.tool_log, "cap %u", c); }
void tool_done(void*, zwp_tablet_tool_v2*) { log(g_c.tool_log, "done"); }
void tool_removed(void*, zwp_tablet_tool_v2*) { log(g_c.tool_log, "removed"); }
void tool_in(void*, zwp_tablet_tool_v2* tool, uint32_t serial, zwp_tablet_v2*, wl_surface* s) {
    g_c.proximity_serial = serial;
    log(g_c.tool_log, "in%s", s == g_c.surface ? "" : " (wrong surface)");
    if (g_c.set_cursor_on_enter) {
        zwp_tablet_tool_v2_set_cursor(tool, serial, g_c.cursor, 3, 4);
        // A stale serial must be ignored.
        zwp_tablet_tool_v2_set_cursor(tool, serial - 1, nullptr, 9, 9);
    }
}
void tool_out(void*, zwp_tablet_tool_v2*) { log(g_c.tool_log, "out"); }
void tool_down(void*, zwp_tablet_tool_v2*, uint32_t) { log(g_c.tool_log, "down"); }
void tool_up(void*, zwp_tablet_tool_v2*) { log(g_c.tool_log, "up"); }
void tool_motion(void*, zwp_tablet_tool_v2*, wl_fixed_t x, wl_fixed_t y) {
    log(g_c.tool_log, "motion %.1f %.1f", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
void tool_pressure(void*, zwp_tablet_tool_v2*, uint32_t p) { log(g_c.tool_log, "pressure %u", p); }
void tool_distance(void*, zwp_tablet_tool_v2*, uint32_t d) { log(g_c.tool_log, "distance %u", d); }
void tool_tilt(void*, zwp_tablet_tool_v2*, wl_fixed_t x, wl_fixed_t y) {
    log(g_c.tool_log, "tilt %.0f %.0f", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
void tool_rotation(void*, zwp_tablet_tool_v2*, wl_fixed_t) { log(g_c.tool_log, "rotation"); }
void tool_slider(void*, zwp_tablet_tool_v2*, int32_t) { log(g_c.tool_log, "slider"); }
void tool_wheel(void*, zwp_tablet_tool_v2*, wl_fixed_t, int32_t) { log(g_c.tool_log, "wheel"); }
void tool_button(void*, zwp_tablet_tool_v2*, uint32_t, uint32_t b, uint32_t state) {
    log(g_c.tool_log, "button %u %u", b, state);
}
void tool_frame(void*, zwp_tablet_tool_v2*, uint32_t) { log(g_c.tool_log, "frame"); }
const zwp_tablet_tool_v2_listener kTool{
    tool_type, tool_serial, tool_wacom, tool_cap,    tool_done,   tool_removed,  tool_in,
    tool_out,  tool_down,   tool_up,    tool_motion, tool_pressure, tool_distance, tool_tilt,
    tool_rotation, tool_slider, tool_wheel, tool_button, tool_frame};

// ---- pad
void ring_source(void*, zwp_tablet_pad_ring_v2*, uint32_t) { log(g_c.pad_log, "ring source"); }
void ring_angle(void*, zwp_tablet_pad_ring_v2*, wl_fixed_t a) {
    log(g_c.pad_log, "ring angle %.0f", wl_fixed_to_double(a));
}
void ring_stop(void*, zwp_tablet_pad_ring_v2*) { log(g_c.pad_log, "ring stop"); }
void ring_frame(void*, zwp_tablet_pad_ring_v2*, uint32_t) { log(g_c.pad_log, "ring frame"); }
const zwp_tablet_pad_ring_v2_listener kRing{ring_source, ring_angle, ring_stop, ring_frame};
void strip_source(void*, zwp_tablet_pad_strip_v2*, uint32_t) { log(g_c.pad_log, "strip source"); }
void strip_position(void*, zwp_tablet_pad_strip_v2*, uint32_t p) {
    log(g_c.pad_log, "strip position %u", p);
}
void strip_stop(void*, zwp_tablet_pad_strip_v2*) { log(g_c.pad_log, "strip stop"); }
void strip_frame(void*, zwp_tablet_pad_strip_v2*, uint32_t) { log(g_c.pad_log, "strip frame"); }
const zwp_tablet_pad_strip_v2_listener kStrip{strip_source, strip_position, strip_stop,
                                              strip_frame};

void group_buttons(void*, zwp_tablet_pad_group_v2*, wl_array* a) {
    log(g_c.pad_log, "group buttons %zu", a->size / sizeof(uint32_t));
}
void group_ring(void*, zwp_tablet_pad_group_v2*, zwp_tablet_pad_ring_v2* r) {
    zwp_tablet_pad_ring_v2_add_listener(r, &kRing, nullptr);
    log(g_c.pad_log, "group ring");
}
void group_strip(void*, zwp_tablet_pad_group_v2*, zwp_tablet_pad_strip_v2* s) {
    zwp_tablet_pad_strip_v2_add_listener(s, &kStrip, nullptr);
    log(g_c.pad_log, "group strip");
}
void group_modes(void*, zwp_tablet_pad_group_v2*, uint32_t m) { log(g_c.pad_log, "group modes %u", m); }
void group_done(void*, zwp_tablet_pad_group_v2*) { log(g_c.pad_log, "group done"); }
void group_mode_switch(void*, zwp_tablet_pad_group_v2*, uint32_t, uint32_t, uint32_t mode) {
    log(g_c.pad_log, "mode %u", mode);
}
void group_dial(void*, zwp_tablet_pad_group_v2*, zwp_tablet_pad_dial_v2*) {}
const zwp_tablet_pad_group_v2_listener kGroup{group_buttons, group_ring, group_strip, group_modes,
                                              group_done,    group_mode_switch, group_dial};

void pad_group(void*, zwp_tablet_pad_v2*, zwp_tablet_pad_group_v2* g) {
    zwp_tablet_pad_group_v2_add_listener(g, &kGroup, nullptr);
}
void pad_path(void*, zwp_tablet_pad_v2*, const char*) {}
void pad_buttons(void*, zwp_tablet_pad_v2*, uint32_t n) { log(g_c.pad_log, "pad buttons %u", n); }
void pad_done(void*, zwp_tablet_pad_v2*) { log(g_c.pad_log, "pad done"); }
void pad_button(void*, zwp_tablet_pad_v2*, uint32_t, uint32_t b, uint32_t state) {
    log(g_c.pad_log, "button %u %u", b, state);
}
void pad_enter(void*, zwp_tablet_pad_v2*, uint32_t, zwp_tablet_v2*, wl_surface*) {
    log(g_c.pad_log, "enter");
}
void pad_leave(void*, zwp_tablet_pad_v2*, uint32_t, wl_surface*) { log(g_c.pad_log, "leave"); }
void pad_removed(void*, zwp_tablet_pad_v2*) { log(g_c.pad_log, "removed"); }
const zwp_tablet_pad_v2_listener kPad{pad_group, pad_path,   pad_buttons, pad_done,
                                      pad_button, pad_enter, pad_leave,   pad_removed};

// ---- seat
void seat_tablet(void*, zwp_tablet_seat_v2*, zwp_tablet_v2* t) {
    zwp_tablet_v2_add_listener(t, &kTablet, nullptr);
}
void seat_tool(void*, zwp_tablet_seat_v2*, zwp_tablet_tool_v2* t) {
    zwp_tablet_tool_v2_add_listener(t, &kTool, nullptr);
}
void seat_pad(void*, zwp_tablet_seat_v2*, zwp_tablet_pad_v2* p) {
    zwp_tablet_pad_v2_add_listener(p, &kPad, nullptr);
}
const zwp_tablet_seat_v2_listener kSeat{seat_tablet, seat_tool, seat_pad};

void xdg_surface_configure(void*, xdg_surface* x, uint32_t serial) {
    xdg_surface_ack_configure(x, serial);
    g_c.configured = true;
}
const xdg_surface_listener kXdgSurface{xdg_surface_configure};
void toplevel_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
void toplevel_close(void*, xdg_toplevel*) {}
void toplevel_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_caps(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener kToplevel{toplevel_configure, toplevel_close, toplevel_bounds,
                                      toplevel_caps};
void wm_ping(void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }
const xdg_wm_base_listener kWm{wm_ping};

void registry_global(void*, wl_registry* reg, uint32_t name, const char* iface, uint32_t) {
    if (!std::strcmp(iface, "wl_compositor")) {
        g_c.compositor = static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (!std::strcmp(iface, "wl_shm")) {
        g_c.shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (!std::strcmp(iface, "wl_seat")) {
        g_c.seat = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, 1));
    } else if (!std::strcmp(iface, "xdg_wm_base")) {
        g_c.wm_base = static_cast<xdg_wm_base*>(wl_registry_bind(reg, name, &xdg_wm_base_interface, 2));
    } else if (!std::strcmp(iface, "zwp_tablet_manager_v2")) {
        g_c.manager = static_cast<zwp_tablet_manager_v2*>(
            wl_registry_bind(reg, name, &zwp_tablet_manager_v2_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

wl_buffer* make_buffer(int w, int h) {
    const int size = w * h * 4;
    int fd = memfd_create("tablet", MFD_CLOEXEC);
    assert(ftruncate(fd, size) == 0);
    wl_shm_pool* pool = wl_shm_create_pool(g_c.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistry, nullptr);
    wl_display_roundtrip(display);
    assert(g_c.compositor && g_c.shm && g_c.seat && g_c.wm_base && g_c.manager);
    xdg_wm_base_add_listener(g_c.wm_base, &kWm, nullptr);

    g_c.surface = wl_compositor_create_surface(g_c.compositor);
    g_c.cursor = wl_compositor_create_surface(g_c.compositor);
    xdg_surface* xs = xdg_wm_base_get_xdg_surface(g_c.wm_base, g_c.surface);
    xdg_surface_add_listener(xs, &kXdgSurface, nullptr);
    xdg_toplevel* tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &kToplevel, nullptr);
    wl_surface_commit(g_c.surface);
    wl_display_roundtrip(display);
    wl_surface_attach(g_c.surface, make_buffer(100, 100), 0, 0);
    wl_surface_commit(g_c.surface);
    wl_display_roundtrip(display);

    g_c.set_cursor_on_enter = true;
    zwp_tablet_seat_v2* tseat = zwp_tablet_manager_v2_get_tablet_seat(g_c.manager, g_c.seat);
    zwp_tablet_seat_v2_add_listener(tseat, &kSeat, nullptr);
    wl_display_roundtrip(display);

    // The server feeds events at its own pace; keep dispatching until it is done.
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        wl_display_roundtrip(display);
    }
    wl_display_disconnect(display);
}

struct DestroyCtx {
    wl_listener listener;
    luminaria::Display* display;
};
void on_client_destroy(wl_listener* l, void*) {
    reinterpret_cast<DestroyCtx*>(reinterpret_cast<char*>(l) - offsetof(DestroyCtx, listener))
        ->display->terminate();
}

bool has(const std::vector<std::string>& log, std::initializer_list<const char*> seq) {
    // `seq` must appear in order, not necessarily adjacent.
    auto it = log.begin();
    for (const char* want : seq) {
        it = std::find(it, log.end(), std::string(want));
        if (it == log.end()) {
            std::fprintf(stderr, "missing \"%s\" in:", want);
            for (const auto& l : log) std::fprintf(stderr, " [%s]", l.c_str());
            std::fprintf(stderr, "\n");
            return false;
        }
        ++it;
    }
    return true;
}

} // namespace

int main() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    assert(display->init_shm().has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto seat = luminaria::Seat::create(*display);
    assert(seat.has_value());
    auto shell = luminaria::XdgShell::create(*display);
    assert(shell.has_value());
    auto tablets = luminaria::TabletManager::create(*display);
    assert(tablets.has_value());

    luminaria::SurfaceId window;
    luminaria::Connections conns;
    conns += shell->new_toplevel().connect([&](luminaria::NewToplevel& e) {
        window = e.toplevel.surface().id();
    });
    luminaria::TabletToolCursorRequest cursor{};
    int cursor_requests = 0;
    conns += tablets->cursor_request().connect([&](luminaria::TabletToolCursorRequest& r) {
        cursor = r;
        ++cursor_requests;
    });

    // The tablet's active area covers a 200x100 layout; the window sits at (10,20).
    const luminaria::Box area{0, 0, 200, 100};
    const luminaria::TabletLocator locate =
        [&](double x, double y) -> std::optional<luminaria::TabletTarget> {
        if (x < 10 || y < 20 || x >= 110 || y >= 120 || !window.valid()) {
            return std::nullopt;
        }
        return luminaria::TabletTarget{window, x - 10, y - 20};
    };
    const auto feed = [&](luminaria::InputEvent e) { tablets->feed(e, area, locate); };

    constexpr std::uint32_t kTablet = 1, kPad = 2;
    constexpr std::uint64_t kPen = 77;
    auto script = display->event_loop().add_timer([&] {
        luminaria::TabletDeviceAdded tablet;
        tablet.device = kTablet;
        tablet.name = "Test Tablet";
        tablet.vid = 0x056a;
        tablet.pid = 0x0378;
        tablet.path = "/dev/input/event99";
        feed(tablet);
        luminaria::TabletDeviceAdded pad;
        pad.device = kPad;
        pad.name = "Test Pad";
        pad.is_pad = true;
        pad.pad.buttons = 4;
        pad.pad.rings = 1;
        pad.pad.strips = 1;
        pad.pad.groups.push_back({.modes = 2, .buttons = {0, 1, 2, 3}, .rings = {0}, .strips = {0}});
        feed(pad);
        assert(tablets->tablet_count() == 1 && tablets->pad_count() == 1);

        // The pen comes into range over the window: (0.5, 0.5) -> (100, 50) -> (90, 30).
        luminaria::TabletToolProximityEvent in;
        in.device = kTablet;
        in.tool = kPen;
        in.in = true;
        in.type = luminaria::TabletToolType::pen;
        in.serial = 0x1234;
        in.caps = {.tilt = true, .pressure = true, .distance = true};
        in.axes = {.changed = luminaria::TabletAxes::everything, .x = 0.5, .y = 0.5,
                   .pressure = 0.0, .distance = 0.25, .tilt_x = 10, .tilt_y = -20,
                   .rotation = 33, .slider = 0.5};
        feed(in);
        assert(tablets->tool_count() == 1);

        // Hover, then press.
        feed(luminaria::TabletToolAxisEvent{
            kTablet, kPen,
            {.changed = luminaria::TabletAxes::position | luminaria::TabletAxes::pressure_axis,
             .x = 0.52, .y = 0.5, .pressure = 0.5}});
        feed(luminaria::TabletToolTipEvent{kTablet, kPen, true, {}});
        feed(luminaria::TabletToolButtonEvent{kTablet, kPen, 0x14b, true, 0});
        feed(luminaria::TabletToolTipEvent{kTablet, kPen, false, {}});

        // Pad: a mode switch with its button, a ring, a strip.
        feed(luminaria::TabletPadButtonEvent{kPad, 2, true, 0, 1, 0});
        feed(luminaria::TabletPadRingEvent{kPad, 0, 90.0, 0});
        feed(luminaria::TabletPadStripEvent{kPad, 0, 0.25, 0});

        // Off the window: (0.9, 0.5) -> (180, 50), outside. Then back: (0.3, 0.7) -> (60, 70).
        feed(luminaria::TabletToolAxisEvent{
            kTablet, kPen, {.changed = luminaria::TabletAxes::position, .x = 0.9, .y = 0.5}});
        feed(luminaria::TabletToolAxisEvent{
            kTablet, kPen, {.changed = luminaria::TabletAxes::position, .x = 0.3, .y = 0.7}});
    });
    script.arm(120);
    auto unplug = display->event_loop().add_timer([&] {
        feed(luminaria::TabletToolProximityEvent{.device = kTablet, .tool = kPen, .in = false});
        feed(luminaria::TabletDeviceRemoved{kTablet});
        assert(tablets->tablet_count() == 0);
    });
    unplug.arm(250);

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);

    std::thread client_thread(run_client, fds[1]);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(5000);
    display->run();
    client_thread.join();

    // Announcements.
    assert(has(g_c.tablet_log, {"name Test Tablet", "id 056a:0378", "path /dev/input/event99",
                                "done", "removed"}));
    assert(has(g_c.tool_log, {"type 140", "serial 0:1234", "cap 1", "cap 2", "cap 3", "done"}));
    assert(!std::ranges::contains(g_c.tool_log, std::string("cap 4"))); // no rotation
    assert(has(g_c.pad_log, {"group buttons 4", "group ring", "group strip", "group modes 2",
                             "group done", "pad buttons 4", "pad done"}));

    // The pen: proximity, the initial state in surface coordinates, only the
    // axes a pen has (no rotation, no slider though the event carried both).
    assert(has(g_c.tool_log, {"in", "motion 90.0 30.0", "pressure 0", "distance 16384", "tilt 10 -20",
                              "frame"}));
    assert(!std::ranges::contains(g_c.tool_log, std::string("rotation")));
    assert(!std::ranges::contains(g_c.tool_log, std::string("slider")));
    assert(has(g_c.tool_log, {"frame", "motion 94.0 30.0", "pressure 32768", "frame", "down",
                              "frame", "button 331 1", "frame", "up", "frame"}));
    // Off the window and back.
    assert(has(g_c.tool_log, {"up", "frame", "out", "frame", "in", "motion 50.0 50.0", "frame",
                              "out", "frame"}));

    // Pad: enters with the pen, mode switch before its button, ring, strip, and
    // leaves when the pen does.
    assert(has(g_c.pad_log, {"enter", "mode 1", "button 2 1", "ring source", "ring angle 90",
                             "ring frame", "strip source", "strip position 16384", "strip frame",
                             "leave", "enter", "leave"}));

    // set_cursor: the proximity serial counts, a stale one does not.
    assert(cursor_requests >= 1);
    assert(cursor.hotspot_x == 3 && cursor.hotspot_y == 4 && cursor.surface.valid());
    assert(tablets->tool_count() == 1);
    return 0;
}
