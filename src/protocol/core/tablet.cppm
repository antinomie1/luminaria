// luminaria/tablet.cppm — zwp_tablet_manager_v2: drawing tablets.
//
// A graphics tablet is not a mouse that happens to be flat. The pen reports how
// hard it presses, how far above the surface it hovers, which way it leans and
// how it is rotated; it has buttons on its barrel and an eraser on the other
// end; and the tablet beside it has express keys, rings and touch strips. A
// painting program (Krita, Blender, Xournal++) wants every one of those, and
// wl_pointer has room for none of them.
//
// There are three kinds of object, and the protocol keeps them apart:
//
//   * a TABLET — the surface a pen is held over;
//   * a TOOL   — the pen, the eraser, a puck: it moves between tablets and is
//                identified by hardware serial, not by where it is;
//   * a PAD    — the express keys, rings and strips, which belong to a tablet
//                but report on their own.
//
// The compositor owns all three (`add_tablet`, `add_tool`, `add_pad`) and every
// client that binds the manager is told about them through its own
// zwp_tablet_seat_v2. Two ways to drive it:
//
//   * `feed(event, area, locate)` takes the backend's `InputEvent`s as they come
//     — devices appearing, tools coming into range, axes, tips, buttons, pads —
//     and does all of it: it maps the tablet's area onto the layout, asks
//     `locate` which surface is there, and sends proximity, motion, pressure,
//     tilt and the rest to that surface's client. This is what a compositor on
//     `LibinputBackend` wants.
//   * the individual `tool_*` / `pad_*` calls, for a compositor that gets its
//     tablet events from somewhere else (a remote protocol, a test).
//
// Focus follows the tool, not the pointer: a tool hovering over a window sends
// proximity-in to that window's client and nothing to anyone else, and
// `proximity_out` when it leaves, is lifted out of range or the surface dies.
//
// The cursor a tool shows is the tool's own business: `set_cursor` from the
// focused client arrives on `cursor_request()`, and a named shape through
// wp_cursor_shape arrives on `CursorShapeManager::request()` with `tablet_tool`
// set, which `TabletManager::tool_id_of()` resolves.

module;

#include "detail/wayland_fwd.h"
#include <cstdint>

#include <wayland-server-core.h>
#include "tablet-v2-protocol.h"

export module luminaria:tablet;

import std;

import :box;
import :compositor;
import :display;
import :expected;
import :input_event;
import :protocol_helper;
import :signal;

export namespace luminaria {

/// Opaque handles. 0 is "none".
using TabletId = std::uint32_t;
using TabletToolId = std::uint32_t;
using TabletPadId = std::uint32_t;

struct TabletInfo {
    std::string name;
    std::uint16_t vid = 0, pid = 0;
    std::string path;
    std::uint32_t bustype = 0; ///< evdev BUS_*, 0 = unknown
};

struct TabletToolInfo {
    TabletToolType type = TabletToolType::pen;
    std::uint64_t hardware_serial = 0; ///< 0 = the pen has none
    std::uint64_t hardware_id_wacom = 0;
    TabletToolCaps caps;
};

struct TabletPadInfo {
    std::string path;
    TabletPadLayout layout;
};

/// The focused client set (or hid) the cursor its tool shows. `surface` is
/// invalid for "hide it". A tool's cursor is drawn at the tool, not at the
/// pointer — compositors usually just treat it like a pointer cursor.
struct TabletToolCursorRequest {
    TabletToolId tool;
    std::uint32_t serial;
    SurfaceId surface;
    int hotspot_x = 0, hotspot_y = 0;
};

/// What `feed()` found under a point of the layout: a surface and the position
/// on it, in surface-local coordinates.
struct TabletTarget {
    SurfaceId surface;
    double x = 0.0, y = 0.0;
};
/// "What is at this point of the layout?" — typically `scene_hit_test()`.
/// Null means nothing, and the tool is out of every client's range there.
using TabletLocator = std::function<std::optional<TabletTarget>(double x, double y)>;

/// The zwp_tablet_manager_v2 global (version 2). Move-only; pointer-stable state.
class TabletManager {
public:
    [[nodiscard]] static Result<TabletManager> create(Display& display);

    ~TabletManager();
    TabletManager(TabletManager&&) noexcept;
    TabletManager& operator=(TabletManager&&) noexcept;
    TabletManager(const TabletManager&) = delete;
    TabletManager& operator=(const TabletManager&) = delete;

    // --- devices ------------------------------------------------------------

    /// A tablet appeared. Every bound client is told at once.
    [[nodiscard]] TabletId add_tablet(const TabletInfo& info);
    /// It was unplugged. Tools in range of it leave first.
    void remove_tablet(TabletId tablet);

    [[nodiscard]] TabletToolId add_tool(const TabletToolInfo& info);
    void remove_tool(TabletToolId tool);

    [[nodiscard]] TabletPadId add_pad(const TabletPadInfo& info);
    void remove_pad(TabletPadId pad);

    [[nodiscard]] std::size_t tablet_count() const noexcept;
    [[nodiscard]] std::size_t tool_count() const noexcept;
    [[nodiscard]] std::size_t pad_count() const noexcept;

    // --- one tool's events --------------------------------------------------
    //
    // Everything below is delivered to the client that owns the surface the tool
    // is over, and dropped when it is over none. A group of changes ends with
    // `tool_frame()`: the client applies them together, as one sample of the pen.

    /// The tool entered `surface`'s client's range over `tablet`.
    void tool_proximity_in(TabletToolId tool, TabletId tablet, Surface& surface);
    void tool_proximity_out(TabletToolId tool);
    /// The surface the tool is over, or invalid.
    [[nodiscard]] SurfaceId tool_focus(TabletToolId tool) const noexcept;

    void tool_down(TabletToolId tool);
    void tool_up(TabletToolId tool);
    /// Surface-local coordinates.
    void tool_motion(TabletToolId tool, double x, double y);
    void tool_pressure(TabletToolId tool, double pressure);   ///< 0..1
    void tool_distance(TabletToolId tool, double distance);   ///< 0..1
    void tool_tilt(TabletToolId tool, double x_deg, double y_deg);
    void tool_rotation(TabletToolId tool, double degrees);
    void tool_slider(TabletToolId tool, double position);     ///< -1..1
    void tool_wheel(TabletToolId tool, double degrees, int clicks);
    /// A barrel button (BTN_STYLUS = 0x14b, BTN_STYLUS2 = 0x14c, ...).
    void tool_button(TabletToolId tool, std::uint32_t button, bool pressed);
    void tool_frame(TabletToolId tool, std::uint32_t time_ms);

    // --- one pad's events ---------------------------------------------------

    /// The pad's events go to this surface's client from now on.
    void pad_enter(TabletPadId pad, TabletId tablet, Surface& surface);
    void pad_leave(TabletPadId pad);
    void pad_button(TabletPadId pad, std::uint32_t time_ms, std::uint32_t button, bool pressed);
    /// `angle` in degrees, or negative for "the finger lifted".
    void pad_ring(TabletPadId pad, std::uint32_t ring, double angle, std::uint32_t time_ms);
    /// `position` 0..1, or negative for "the finger lifted".
    void pad_strip(TabletPadId pad, std::uint32_t strip, double position, std::uint32_t time_ms);
    void pad_mode_switch(TabletPadId pad, std::uint32_t group, std::uint32_t mode,
                         std::uint32_t time_ms);

    // --- the whole thing at once ---------------------------------------------

    /// Route one backend event. Devices and tools are created, retired and
    /// tracked by themselves; positions map `area` (a box in layout
    /// coordinates) to the tablet's [0,1] square; `locate` says what is under a
    /// point. Events that are not tablet events are ignored, so a compositor can
    /// hand it every `InputEvent` it gets.
    ///
    /// While a tool hovers, `locate` is asked on every motion, so a pen that
    /// slides from one window to another changes focus by itself. Pads follow
    /// the tools: while any tool is over a surface, every pad is focused on it.
    void feed(const InputEvent& event, const Box& area, const TabletLocator& locate);

    // --- what clients ask ----------------------------------------------------

    /// A client set or hid the cursor its tool shows.
    [[nodiscard]] Signal<TabletToolCursorRequest>& cursor_request() noexcept;

    /// Which tool a zwp_tablet_tool_v2 resource is, or 0 for anything else.
    /// For wp_cursor_shape's `get_tablet_tool_v2`.
    [[nodiscard]] static TabletToolId tool_id_of(wl_resource* tablet_tool) noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit TabletManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
//
// Same shape as foreign_toplevel.cppm: the devices live once in Impl, and every
// bound zwp_tablet_seat_v2 (one per client binding) gets its own resource for
// each of them. A tool's events go only to the bindings of the client that owns
// its focus surface.

namespace luminaria {

struct TabletManager::Impl {
    WlGlobal global;
    wl_display* display = nullptr;
    Signal<TabletToolCursorRequest> cursor_request;

    struct Tablet {
        TabletInfo info;
    };
    struct Tool {
        TabletToolInfo info;
        SurfaceId focus;
        TabletId focus_tablet = 0;
        std::uint32_t serial = 0; // the proximity_in serial: what set_cursor answers
        bool down = false;
    };
    struct Pad {
        TabletPadInfo info;
        SurfaceId focus;
    };
    std::map<TabletId, Tablet> tablets;
    std::map<TabletToolId, Tool> tools;
    std::map<TabletPadId, Pad> pads;
    std::uint32_t next_id = 1;

    /// One client's view of one pad: the pad, its groups, and the ring and strip
    /// objects its groups handed out (indexed by the pad's own ring/strip number).
    struct PadRes {
        wl_resource* pad = nullptr;
        std::vector<wl_resource*> groups;
        std::vector<wl_resource*> rings, strips;
    };
    /// One bound zwp_tablet_seat_v2.
    struct Binding {
        wl_resource* resource = nullptr;
        Impl* impl = nullptr;
        std::map<TabletId, wl_resource*> tablets;
        std::map<TabletToolId, wl_resource*> tools;
        std::map<TabletPadId, PadRes> pads;
    };
    std::vector<std::unique_ptr<Binding>> bindings;

    // --- feed() state, keyed by the backend's own ids -------------------------
    struct FedTool {
        TabletToolId id = 0;
        TabletId tablet = 0; // the tablet it is over
        bool in_range = false;
        double x = 0, y = 0; // last position in layout coordinates
        bool down = false;
    };
    std::map<std::uint32_t, TabletId> fed_tablets;
    std::map<std::uint32_t, TabletPadId> fed_pads;
    std::map<std::uint64_t, FedTool> fed_tools;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> fed_modes; // (device, group)

    [[nodiscard]] std::uint32_t next_serial() const { return wl_display_next_serial(display); }

    /// The client behind a surface, or null when it has gone.
    static wl_client* client_of_surface(SurfaceId id) {
        Surface* surface = surface_from_id(id);
        return surface != nullptr ? wl_resource_get_client(surface->c_resource()) : nullptr;
    }

    /// Run `fn(tool_resource)` for the focused client's resource of this tool.
    template <class Fn>
    void to_tool_focus(TabletToolId id, Fn&& fn) {
        auto it = tools.find(id);
        if (it == tools.end()) {
            return;
        }
        wl_client* client = client_of_surface(it->second.focus);
        if (client == nullptr) {
            it->second.focus = SurfaceId{}; // the surface (or its client) is gone
            return;
        }
        for (auto& b : bindings) {
            if (wl_resource_get_client(b->resource) != client) {
                continue;
            }
            if (auto res = b->tools.find(id); res != b->tools.end()) {
                fn(res->second);
            }
        }
    }

    template <class Fn>
    void to_pad_focus(TabletPadId id, Fn&& fn) {
        auto it = pads.find(id);
        if (it == pads.end()) {
            return;
        }
        wl_client* client = client_of_surface(it->second.focus);
        if (client == nullptr) {
            it->second.focus = SurfaceId{};
            return;
        }
        for (auto& b : bindings) {
            if (wl_resource_get_client(b->resource) != client) {
                continue;
            }
            if (auto res = b->pads.find(id); res != b->pads.end()) {
                fn(res->second);
            }
        }
    }

    void send_tablet(Binding& b, TabletId id, const Tablet& t);
    void send_tool(Binding& b, TabletToolId id, const Tool& t);
    void send_pad(Binding& b, TabletPadId id, const Pad& p);

    void feed_tool_event(const InputEvent& event, const Box& area, const TabletLocator& locate,
                         TabletManager& self);

    ~Impl();
};

namespace {

using TabImpl = TabletManager::Impl;
using TabBinding = TabImpl::Binding;

enum class TabKind { tablet, tool, pad, group, ring, strip };

/// user_data of every resource a client holds. `binding` is nulled when the
/// seat that made it goes (the protocol leaves the child alive, but inert) and
/// when the manager itself does.
struct TabChild {
    TabBinding* binding = nullptr;
    TabKind kind = TabKind::tablet;
    std::uint32_t id = 0;    // the tablet / tool / pad it belongs to
    std::uint32_t index = 0; // ring / strip number
};

TabChild* child_of(wl_resource* r) { return static_cast<TabChild*>(wl_resource_get_user_data(r)); }

void child_destroyed(wl_resource* resource) {
    TabChild* c = child_of(resource);
    if (c->binding != nullptr) {
        switch (c->kind) {
        case TabKind::tablet:
            c->binding->tablets.erase(c->id);
            break;
        case TabKind::tool:
            c->binding->tools.erase(c->id);
            break;
        case TabKind::pad:
            c->binding->pads.erase(c->id);
            break;
        case TabKind::ring:
        case TabKind::strip:
            if (auto it = c->binding->pads.find(c->id); it != c->binding->pads.end()) {
                auto& list = c->kind == TabKind::ring ? it->second.rings : it->second.strips;
                if (c->index < list.size() && list[c->index] == resource) {
                    list[c->index] = nullptr;
                }
            }
            break;
        case TabKind::group:
            if (auto it = c->binding->pads.find(c->id); it != c->binding->pads.end()) {
                // Blank, not erase: the mode-switch group index is a position.
                std::ranges::replace(it->second.groups, resource, nullptr);
            }
            break;
        }
    }
    delete c;
}

wl_resource* make_child(wl_client* client, const wl_interface* iface, const void* vtable,
                        std::uint32_t version, TabBinding* binding, TabKind kind, std::uint32_t id,
                        std::uint32_t index = 0) {
    wl_resource* resource = wl_resource_create(client, iface, static_cast<int>(version), 0);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return nullptr;
    }
    wl_resource_set_implementation(resource, vtable, new TabChild{binding, kind, id, index},
                                   child_destroyed);
    return resource;
}

// ---- requests --------------------------------------------------------------

// A tablet and a ring/strip/group have nothing a client can ask for but `destroy`.
constexpr struct zwp_tablet_v2_interface tablet_impl = {.destroy = resource_destroy_request};
constexpr struct zwp_tablet_pad_group_v2_interface group_impl = {
    .destroy = resource_destroy_request};

// Feedback strings label the buttons on the tablet's own display (some Wacom
// models have them). Nothing here drives such a display, so they are accepted
// and dropped.
void ring_set_feedback(wl_client*, wl_resource*, const char*, uint32_t) {}
constexpr struct zwp_tablet_pad_ring_v2_interface ring_impl = {
    .set_feedback = ring_set_feedback, .destroy = resource_destroy_request};
constexpr struct zwp_tablet_pad_strip_v2_interface strip_impl = {
    .set_feedback = ring_set_feedback, .destroy = resource_destroy_request};
void pad_set_feedback(wl_client*, wl_resource*, uint32_t, const char*, uint32_t) {}
constexpr struct zwp_tablet_pad_v2_interface pad_impl = {.set_feedback = pad_set_feedback,
                                                         .destroy = resource_destroy_request};

void tool_set_cursor(wl_client* client, wl_resource* resource, uint32_t serial,
                     wl_resource* surface_resource, int32_t hotspot_x, int32_t hotspot_y) {
    TabChild* c = child_of(resource);
    if (c->binding == nullptr || c->binding->impl == nullptr) {
        return;
    }
    TabImpl& impl = *c->binding->impl;
    auto it = impl.tools.find(c->id);
    // Only the client that holds the tool's focus, answering the serial it was
    // given at proximity-in, may say what the cursor looks like.
    if (it == impl.tools.end() || it->second.serial != serial ||
        TabImpl::client_of_surface(it->second.focus) != client) {
        return;
    }
    Surface* surface = surface_resource != nullptr ? surface_from_resource(surface_resource) : nullptr;
    if (surface != nullptr) {
        // The cursor rides the tool, so a hit test under the tool would
        // otherwise find the cursor itself instead of the window beneath.
        surface->set_input_transparent();
    }
    TabletToolCursorRequest request{c->id, serial, surface != nullptr ? surface->id() : SurfaceId{},
                                    hotspot_x, hotspot_y};
    impl.cursor_request.emit(request);
}
constexpr struct zwp_tablet_tool_v2_interface tool_impl = {.set_cursor = tool_set_cursor,
                                                           .destroy = resource_destroy_request};

void seat_destroy_request(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
constexpr struct zwp_tablet_seat_v2_interface seat_impl = {.destroy = seat_destroy_request};

/// Sever every child of a binding. They stay valid objects the client may still
/// destroy; they just never hear anything again.
void sever(TabBinding& b) {
    const auto cut = [](wl_resource* r) {
        if (r != nullptr) {
            child_of(r)->binding = nullptr;
        }
    };
    for (auto& [id, r] : b.tablets) {
        cut(r);
    }
    for (auto& [id, r] : b.tools) {
        cut(r);
    }
    for (auto& [id, pad] : b.pads) {
        cut(pad.pad);
        for (wl_resource* r : pad.groups) {
            cut(r);
        }
        for (wl_resource* r : pad.rings) {
            cut(r);
        }
        for (wl_resource* r : pad.strips) {
            cut(r);
        }
    }
    b.tablets.clear();
    b.tools.clear();
    b.pads.clear();
}

void seat_resource_destroy(wl_resource* resource) {
    auto* b = static_cast<TabBinding*>(wl_resource_get_user_data(resource));
    if (b == nullptr || b->impl == nullptr) {
        return;
    }
    sever(*b);
    std::erase_if(b->impl->bindings,
                  [b](const std::unique_ptr<TabBinding>& held) { return held.get() == b; });
}

void manager_get_tablet_seat(wl_client* client, wl_resource* manager, uint32_t id,
                             wl_resource* /*seat*/) {
    // One seat in this library, so which wl_seat the client names is no question.
    auto* impl = static_cast<TabImpl*>(wl_resource_get_user_data(manager));
    wl_resource* resource = wl_resource_create(client, &zwp_tablet_seat_v2_interface,
                                               wl_resource_get_version(manager), id);
    if (resource == nullptr) {
        wl_resource_post_no_memory(manager);
        return;
    }
    auto binding = std::make_unique<TabBinding>();
    binding->resource = resource;
    binding->impl = impl;
    TabBinding* b = binding.get();
    impl->bindings.push_back(std::move(binding));
    wl_resource_set_implementation(resource, &seat_impl, b, seat_resource_destroy);

    // A client that binds late still gets everything that exists.
    for (auto& [tid, t] : impl->tablets) {
        impl->send_tablet(*b, tid, t);
    }
    for (auto& [tid, t] : impl->tools) {
        impl->send_tool(*b, tid, t);
    }
    for (auto& [pid, p] : impl->pads) {
        impl->send_pad(*b, pid, p);
    }
}

constexpr struct zwp_tablet_manager_v2_interface manager_impl = {
    .get_tablet_seat = manager_get_tablet_seat,
    .destroy = resource_destroy_request,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zwp_tablet_manager_v2_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

wl_fixed_t fixed(double v) { return wl_fixed_from_double(v); }
std::uint32_t unit16(double v) {
    return static_cast<std::uint32_t>(std::clamp(v, 0.0, 1.0) * 65535.0 + 0.5);
}

} // namespace

TabImpl::~Impl() {
    // The seats and everything under them are the client's and outlive us at
    // exit (the Display, declared first, is what destroys a still-connected
    // client). Leave them inert.
    for (auto& b : bindings) {
        sever(*b);
        b->impl = nullptr;
        detach_resource(b->resource);
    }
}

void TabImpl::send_tablet(Binding& b, TabletId id, const Tablet& t) {
    wl_client* client = wl_resource_get_client(b.resource);
    const std::uint32_t version = wl_resource_get_version(b.resource);
    wl_resource* res =
        make_child(client, &zwp_tablet_v2_interface, &tablet_impl, version, &b, TabKind::tablet, id);
    if (res == nullptr) {
        return;
    }
    b.tablets[id] = res;
    zwp_tablet_seat_v2_send_tablet_added(b.resource, res);
    zwp_tablet_v2_send_name(res, t.info.name.c_str());
    zwp_tablet_v2_send_id(res, t.info.vid, t.info.pid);
    if (!t.info.path.empty()) {
        zwp_tablet_v2_send_path(res, t.info.path.c_str());
    }
    if (t.info.bustype != 0 && version >= ZWP_TABLET_V2_BUSTYPE_SINCE_VERSION) {
        zwp_tablet_v2_send_bustype(res, t.info.bustype);
    }
    zwp_tablet_v2_send_done(res);
}

void TabImpl::send_tool(Binding& b, TabletToolId id, const Tool& t) {
    wl_client* client = wl_resource_get_client(b.resource);
    wl_resource* res = make_child(client, &zwp_tablet_tool_v2_interface, &tool_impl,
                                  wl_resource_get_version(b.resource), &b, TabKind::tool, id);
    if (res == nullptr) {
        return;
    }
    b.tools[id] = res;
    zwp_tablet_seat_v2_send_tool_added(b.resource, res);
    zwp_tablet_tool_v2_send_type(res, static_cast<std::uint32_t>(t.info.type));
    if (t.info.hardware_serial != 0) {
        zwp_tablet_tool_v2_send_hardware_serial(
            res, static_cast<std::uint32_t>(t.info.hardware_serial >> 32),
            static_cast<std::uint32_t>(t.info.hardware_serial & 0xffffffffu));
    }
    if (t.info.hardware_id_wacom != 0) {
        zwp_tablet_tool_v2_send_hardware_id_wacom(
            res, static_cast<std::uint32_t>(t.info.hardware_id_wacom >> 32),
            static_cast<std::uint32_t>(t.info.hardware_id_wacom & 0xffffffffu));
    }
    const TabletToolCaps& c = t.info.caps;
    if (c.tilt) {
        zwp_tablet_tool_v2_send_capability(res, ZWP_TABLET_TOOL_V2_CAPABILITY_TILT);
    }
    if (c.pressure) {
        zwp_tablet_tool_v2_send_capability(res, ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE);
    }
    if (c.distance) {
        zwp_tablet_tool_v2_send_capability(res, ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE);
    }
    if (c.rotation) {
        zwp_tablet_tool_v2_send_capability(res, ZWP_TABLET_TOOL_V2_CAPABILITY_ROTATION);
    }
    if (c.slider) {
        zwp_tablet_tool_v2_send_capability(res, ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER);
    }
    if (c.wheel) {
        zwp_tablet_tool_v2_send_capability(res, ZWP_TABLET_TOOL_V2_CAPABILITY_WHEEL);
    }
    zwp_tablet_tool_v2_send_done(res);
}

void TabImpl::send_pad(Binding& b, TabletPadId id, const Pad& p) {
    wl_client* client = wl_resource_get_client(b.resource);
    const std::uint32_t version = wl_resource_get_version(b.resource);
    wl_resource* res =
        make_child(client, &zwp_tablet_pad_v2_interface, &pad_impl, version, &b, TabKind::pad, id);
    if (res == nullptr) {
        return;
    }
    PadRes& pr = b.pads[id];
    pr.pad = res;
    const TabletPadLayout& layout = p.info.layout;
    pr.rings.assign(layout.rings, nullptr);
    pr.strips.assign(layout.strips, nullptr);
    zwp_tablet_seat_v2_send_pad_added(b.resource, res);

    // Groups come first, each with its own buttons / rings / strips / modes,
    // and a `done` of its own; then the pad's own path, button count and `done`.
    std::vector<TabletPadGroup> groups = layout.groups;
    if (groups.empty()) {
        // A pad nobody described: one group that owns every control, one mode.
        TabletPadGroup all;
        for (std::uint32_t i = 0; i < layout.buttons; ++i) {
            all.buttons.push_back(i);
        }
        for (std::uint32_t i = 0; i < layout.rings; ++i) {
            all.rings.push_back(i);
        }
        for (std::uint32_t i = 0; i < layout.strips; ++i) {
            all.strips.push_back(i);
        }
        groups.push_back(std::move(all));
    }
    for (const TabletPadGroup& g : groups) {
        wl_resource* group = make_child(client, &zwp_tablet_pad_group_v2_interface, &group_impl,
                                        version, &b, TabKind::group, id);
        if (group == nullptr) {
            continue;
        }
        pr.groups.push_back(group);
        zwp_tablet_pad_v2_send_group(res, group);
        wl_array buttons;
        wl_array_init(&buttons);
        for (std::uint32_t button : g.buttons) {
            if (auto* slot = static_cast<std::uint32_t*>(wl_array_add(&buttons, sizeof(std::uint32_t)));
                slot != nullptr) {
                *slot = button;
            }
        }
        zwp_tablet_pad_group_v2_send_buttons(group, &buttons);
        wl_array_release(&buttons);
        for (std::uint32_t ring : g.rings) {
            if (ring >= pr.rings.size()) {
                continue;
            }
            wl_resource* r = make_child(client, &zwp_tablet_pad_ring_v2_interface, &ring_impl,
                                        version, &b, TabKind::ring, id, ring);
            if (r != nullptr) {
                pr.rings[ring] = r;
                zwp_tablet_pad_group_v2_send_ring(group, r);
            }
        }
        for (std::uint32_t strip : g.strips) {
            if (strip >= pr.strips.size()) {
                continue;
            }
            wl_resource* r = make_child(client, &zwp_tablet_pad_strip_v2_interface, &strip_impl,
                                        version, &b, TabKind::strip, id, strip);
            if (r != nullptr) {
                pr.strips[strip] = r;
                zwp_tablet_pad_group_v2_send_strip(group, r);
            }
        }
        zwp_tablet_pad_group_v2_send_modes(group, std::max(1u, g.modes));
        zwp_tablet_pad_group_v2_send_done(group);
    }
    if (!p.info.path.empty()) {
        zwp_tablet_pad_v2_send_path(res, p.info.path.c_str());
    }
    zwp_tablet_pad_v2_send_buttons(res, layout.buttons);
    zwp_tablet_pad_v2_send_done(res);
}

// ---------------------------------------------------------------- public API

TabletManager::TabletManager(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TabletManager::~TabletManager() = default;
TabletManager::TabletManager(TabletManager&&) noexcept = default;
TabletManager& TabletManager::operator=(TabletManager&&) noexcept = default;

Result<TabletManager> TabletManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    auto global = create_wl_global<&zwp_tablet_manager_v2_interface, manager_bind>(display, 2,
                                                                                    impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return TabletManager{std::move(impl)};
}

TabletId TabletManager::add_tablet(const TabletInfo& info) {
    const TabletId id = impl_->next_id++;
    auto& tablet = impl_->tablets[id] = Impl::Tablet{info};
    for (auto& b : impl_->bindings) {
        impl_->send_tablet(*b, id, tablet);
    }
    return id;
}

void TabletManager::remove_tablet(TabletId tablet) {
    if (!impl_->tablets.contains(tablet)) {
        return;
    }
    // A tool over a tablet that is gone has nothing to be over.
    std::vector<TabletToolId> leaving;
    for (auto& [id, tool] : impl_->tools) {
        if (tool.focus_tablet == tablet && tool.focus.valid()) {
            leaving.push_back(id);
        }
    }
    for (TabletToolId id : leaving) {
        tool_proximity_out(id);
    }
    for (auto& b : impl_->bindings) {
        if (auto it = b->tablets.find(tablet); it != b->tablets.end()) {
            zwp_tablet_v2_send_removed(it->second);
            b->tablets.erase(it);
        }
    }
    impl_->tablets.erase(tablet);
}

TabletToolId TabletManager::add_tool(const TabletToolInfo& info) {
    const TabletToolId id = impl_->next_id++;
    auto& tool = impl_->tools[id];
    tool.info = info;
    for (auto& b : impl_->bindings) {
        impl_->send_tool(*b, id, tool);
    }
    return id;
}

void TabletManager::remove_tool(TabletToolId tool) {
    if (!impl_->tools.contains(tool)) {
        return;
    }
    tool_proximity_out(tool);
    for (auto& b : impl_->bindings) {
        if (auto it = b->tools.find(tool); it != b->tools.end()) {
            zwp_tablet_tool_v2_send_removed(it->second);
            b->tools.erase(it);
        }
    }
    impl_->tools.erase(tool);
}

TabletPadId TabletManager::add_pad(const TabletPadInfo& info) {
    const TabletPadId id = impl_->next_id++;
    auto& pad = impl_->pads[id];
    pad.info = info;
    for (auto& b : impl_->bindings) {
        impl_->send_pad(*b, id, pad);
    }
    return id;
}

void TabletManager::remove_pad(TabletPadId pad) {
    if (!impl_->pads.contains(pad)) {
        return;
    }
    pad_leave(pad);
    for (auto& b : impl_->bindings) {
        if (auto it = b->pads.find(pad); it != b->pads.end()) {
            zwp_tablet_pad_v2_send_removed(it->second.pad);
            b->pads.erase(it);
        }
    }
    impl_->pads.erase(pad);
}

std::size_t TabletManager::tablet_count() const noexcept { return impl_->tablets.size(); }
std::size_t TabletManager::tool_count() const noexcept { return impl_->tools.size(); }
std::size_t TabletManager::pad_count() const noexcept { return impl_->pads.size(); }

// ---- tools

void TabletManager::tool_proximity_in(TabletToolId id, TabletId tablet, Surface& surface) {
    auto it = impl_->tools.find(id);
    if (it == impl_->tools.end() || !impl_->tablets.contains(tablet)) {
        return;
    }
    Impl::Tool& tool = it->second;
    if (tool.focus == surface.id() && tool.focus_tablet == tablet) {
        return;
    }
    if (tool.focus.valid()) {
        tool_proximity_out(id);
    }
    wl_client* client = wl_resource_get_client(surface.c_resource());
    const std::uint32_t serial = impl_->next_serial();
    bool sent = false;
    for (auto& b : impl_->bindings) {
        if (wl_resource_get_client(b->resource) != client) {
            continue;
        }
        auto tool_res = b->tools.find(id);
        auto tablet_res = b->tablets.find(tablet);
        if (tool_res == b->tools.end() || tablet_res == b->tablets.end()) {
            continue;
        }
        zwp_tablet_tool_v2_send_proximity_in(tool_res->second, serial, tablet_res->second,
                                             surface.c_resource());
        sent = true;
    }
    if (sent) {
        tool.focus = surface.id();
        tool.focus_tablet = tablet;
        tool.serial = serial;
    }
}

void TabletManager::tool_proximity_out(TabletToolId id) {
    auto it = impl_->tools.find(id);
    if (it == impl_->tools.end() || !it->second.focus.valid()) {
        return;
    }
    Impl::Tool& tool = it->second;
    if (tool.down) {
        tool_up(id);
    }
    // The frame closes the sample proximity_out is part of, so nobody who calls
    // this has to remember it.
    const auto now = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    impl_->to_tool_focus(id, [now](wl_resource* r) {
        zwp_tablet_tool_v2_send_proximity_out(r);
        zwp_tablet_tool_v2_send_frame(r, now);
    });
    tool.focus = SurfaceId{};
    tool.focus_tablet = 0;
}

SurfaceId TabletManager::tool_focus(TabletToolId tool) const noexcept {
    auto it = impl_->tools.find(tool);
    return it != impl_->tools.end() ? it->second.focus : SurfaceId{};
}

void TabletManager::tool_down(TabletToolId id) {
    auto it = impl_->tools.find(id);
    if (it == impl_->tools.end() || it->second.down) {
        return;
    }
    const std::uint32_t serial = impl_->next_serial();
    it->second.down = true;
    impl_->to_tool_focus(id, [serial](wl_resource* r) { zwp_tablet_tool_v2_send_down(r, serial); });
}

void TabletManager::tool_up(TabletToolId id) {
    auto it = impl_->tools.find(id);
    if (it == impl_->tools.end() || !it->second.down) {
        return;
    }
    it->second.down = false;
    impl_->to_tool_focus(id, [](wl_resource* r) { zwp_tablet_tool_v2_send_up(r); });
}

void TabletManager::tool_motion(TabletToolId id, double x, double y) {
    impl_->to_tool_focus(id, [x, y](wl_resource* r) {
        zwp_tablet_tool_v2_send_motion(r, fixed(x), fixed(y));
    });
}

void TabletManager::tool_pressure(TabletToolId id, double pressure) {
    impl_->to_tool_focus(id, [pressure](wl_resource* r) {
        zwp_tablet_tool_v2_send_pressure(r, unit16(pressure));
    });
}

void TabletManager::tool_distance(TabletToolId id, double distance) {
    impl_->to_tool_focus(id, [distance](wl_resource* r) {
        zwp_tablet_tool_v2_send_distance(r, unit16(distance));
    });
}

void TabletManager::tool_tilt(TabletToolId id, double x_deg, double y_deg) {
    impl_->to_tool_focus(id, [x_deg, y_deg](wl_resource* r) {
        zwp_tablet_tool_v2_send_tilt(r, fixed(x_deg), fixed(y_deg));
    });
}

void TabletManager::tool_rotation(TabletToolId id, double degrees) {
    impl_->to_tool_focus(id, [degrees](wl_resource* r) {
        zwp_tablet_tool_v2_send_rotation(r, fixed(degrees));
    });
}

void TabletManager::tool_slider(TabletToolId id, double position) {
    impl_->to_tool_focus(id, [position](wl_resource* r) {
        zwp_tablet_tool_v2_send_slider(
            r, static_cast<std::int32_t>(std::clamp(position, -1.0, 1.0) * 65535.0));
    });
}

void TabletManager::tool_wheel(TabletToolId id, double degrees, int clicks) {
    impl_->to_tool_focus(id, [degrees, clicks](wl_resource* r) {
        zwp_tablet_tool_v2_send_wheel(r, fixed(degrees), clicks);
    });
}

void TabletManager::tool_button(TabletToolId id, std::uint32_t button, bool pressed) {
    const std::uint32_t serial = impl_->next_serial();
    impl_->to_tool_focus(id, [serial, button, pressed](wl_resource* r) {
        zwp_tablet_tool_v2_send_button(r, serial, button,
                                       pressed ? ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED
                                               : ZWP_TABLET_TOOL_V2_BUTTON_STATE_RELEASED);
    });
}

void TabletManager::tool_frame(TabletToolId id, std::uint32_t time_ms) {
    impl_->to_tool_focus(id, [time_ms](wl_resource* r) { zwp_tablet_tool_v2_send_frame(r, time_ms); });
}

// ---- pads

void TabletManager::pad_enter(TabletPadId id, TabletId tablet, Surface& surface) {
    auto it = impl_->pads.find(id);
    if (it == impl_->pads.end() || !impl_->tablets.contains(tablet)) {
        return;
    }
    if (it->second.focus == surface.id()) {
        return;
    }
    pad_leave(id);
    wl_client* client = wl_resource_get_client(surface.c_resource());
    const std::uint32_t serial = impl_->next_serial();
    bool sent = false;
    for (auto& b : impl_->bindings) {
        if (wl_resource_get_client(b->resource) != client) {
            continue;
        }
        auto pad_res = b->pads.find(id);
        auto tablet_res = b->tablets.find(tablet);
        if (pad_res == b->pads.end() || tablet_res == b->tablets.end()) {
            continue;
        }
        zwp_tablet_pad_v2_send_enter(pad_res->second.pad, serial, tablet_res->second,
                                     surface.c_resource());
        sent = true;
    }
    if (sent) {
        it->second.focus = surface.id();
    }
}

void TabletManager::pad_leave(TabletPadId id) {
    auto it = impl_->pads.find(id);
    if (it == impl_->pads.end() || !it->second.focus.valid()) {
        return;
    }
    Surface* surface = surface_from_id(it->second.focus);
    const std::uint32_t serial = impl_->next_serial();
    if (surface != nullptr) {
        impl_->to_pad_focus(id, [serial, surface](Impl::PadRes& pr) {
            zwp_tablet_pad_v2_send_leave(pr.pad, serial, surface->c_resource());
        });
    }
    it->second.focus = SurfaceId{};
}

void TabletManager::pad_button(TabletPadId id, std::uint32_t time_ms, std::uint32_t button,
                               bool pressed) {
    impl_->to_pad_focus(id, [=](Impl::PadRes& pr) {
        zwp_tablet_pad_v2_send_button(pr.pad, time_ms, button,
                                      pressed ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
                                              : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
    });
}

void TabletManager::pad_ring(TabletPadId id, std::uint32_t ring, double angle,
                             std::uint32_t time_ms) {
    impl_->to_pad_focus(id, [=](Impl::PadRes& pr) {
        if (ring >= pr.rings.size() || pr.rings[ring] == nullptr) {
            return;
        }
        wl_resource* r = pr.rings[ring];
        zwp_tablet_pad_ring_v2_send_source(r, ZWP_TABLET_PAD_RING_V2_SOURCE_FINGER);
        if (angle < 0.0) {
            zwp_tablet_pad_ring_v2_send_stop(r);
        } else {
            zwp_tablet_pad_ring_v2_send_angle(r, fixed(angle));
        }
        zwp_tablet_pad_ring_v2_send_frame(r, time_ms);
    });
}

void TabletManager::pad_strip(TabletPadId id, std::uint32_t strip, double position,
                              std::uint32_t time_ms) {
    impl_->to_pad_focus(id, [=](Impl::PadRes& pr) {
        if (strip >= pr.strips.size() || pr.strips[strip] == nullptr) {
            return;
        }
        wl_resource* r = pr.strips[strip];
        zwp_tablet_pad_strip_v2_send_source(r, ZWP_TABLET_PAD_STRIP_V2_SOURCE_FINGER);
        if (position < 0.0) {
            zwp_tablet_pad_strip_v2_send_stop(r);
        } else {
            zwp_tablet_pad_strip_v2_send_position(r, unit16(position));
        }
        zwp_tablet_pad_strip_v2_send_frame(r, time_ms);
    });
}

void TabletManager::pad_mode_switch(TabletPadId id, std::uint32_t group, std::uint32_t mode,
                                    std::uint32_t time_ms) {
    const std::uint32_t serial = impl_->next_serial();
    impl_->to_pad_focus(id, [=](Impl::PadRes& pr) {
        if (group < pr.groups.size() && pr.groups[group] != nullptr) {
            zwp_tablet_pad_group_v2_send_mode_switch(pr.groups[group], time_ms, serial, mode);
        }
    });
}

Signal<TabletToolCursorRequest>& TabletManager::cursor_request() noexcept {
    return impl_->cursor_request;
}

TabletToolId TabletManager::tool_id_of(wl_resource* tablet_tool) noexcept {
    if (tablet_tool == nullptr ||
        !wl_resource_instance_of(tablet_tool, &zwp_tablet_tool_v2_interface, &tool_impl)) {
        return 0;
    }
    return child_of(tablet_tool)->id;
}

// ---- feed()

void TabletManager::feed(const InputEvent& event, const Box& area, const TabletLocator& locate) {
    impl_->feed_tool_event(event, area, locate, *this);
}

void TabImpl::feed_tool_event(const InputEvent& event, const Box& area, const TabletLocator& locate,
                              TabletManager& self) {
    // Layout position of a normalised tablet position.
    const auto to_layout = [&area](double x, double y) {
        return std::pair{area.x + x * area.width, area.y + y * area.height};
    };
    const auto any_focus = [this]() -> Surface* {
        for (auto& [id, tool] : tools) {
            if (Surface* s = surface_from_id(tool.focus); s != nullptr) {
                return s;
            }
        }
        return nullptr;
    };
    // The pads follow the tools: while a tool is over a surface, every pad is
    // focused on it; once none is, they leave.
    const auto sync_pads = [&](TabletId over) {
        Surface* surface = any_focus();
        for (auto& [backend, pad] : fed_pads) {
            if (surface != nullptr) {
                self.pad_enter(pad, over, *surface);
            } else {
                self.pad_leave(pad);
            }
        }
    };
    // Put the tool where `axes` says, sending proximity in / out as it crosses
    // from one surface to another. Returns the target it ended up over.
    const auto retarget = [&](FedTool& tool, const TabletAxes& axes) -> std::optional<TabletTarget> {
        if ((axes.changed & TabletAxes::position) != 0) {
            std::tie(tool.x, tool.y) = to_layout(axes.x, axes.y);
        }
        std::optional<TabletTarget> target;
        if (tool.in_range) {
            target = locate(tool.x, tool.y);
        }
        const SurfaceId before = self.tool_focus(tool.id);
        if (!target.has_value()) {
            if (before.valid()) {
                self.tool_proximity_out(tool.id);
                sync_pads(tool.tablet);
            }
            return std::nullopt;
        }
        if (before != target->surface) {
            if (Surface* s = surface_from_id(target->surface); s != nullptr) {
                self.tool_proximity_in(tool.id, tool.tablet, *s);
                sync_pads(tool.tablet);
            }
        }
        return target;
    };
    const auto send_axes = [&](FedTool& tool, const TabletAxes& axes,
                               const std::optional<TabletTarget>& target, bool entered,
                               std::uint32_t time) {
        if (!target.has_value() || !self.tool_focus(tool.id).valid()) {
            return;
        }
        // Only the axes this tool has: a client told about a pressure a pen
        // cannot measure would trust it.
        const TabletToolCaps caps = tools[tool.id].info.caps;
        if ((axes.changed & TabletAxes::position) != 0 || entered) {
            self.tool_motion(tool.id, target->x, target->y);
        }
        if (caps.pressure && (axes.changed & TabletAxes::pressure_axis) != 0) {
            self.tool_pressure(tool.id, axes.pressure);
        }
        if (caps.distance && (axes.changed & TabletAxes::distance_axis) != 0) {
            self.tool_distance(tool.id, axes.distance);
        }
        if (caps.tilt && (axes.changed & TabletAxes::tilt) != 0) {
            self.tool_tilt(tool.id, axes.tilt_x, axes.tilt_y);
        }
        if (caps.rotation && (axes.changed & TabletAxes::rotation_axis) != 0) {
            self.tool_rotation(tool.id, axes.rotation);
        }
        if (caps.slider && (axes.changed & TabletAxes::slider_axis) != 0) {
            self.tool_slider(tool.id, axes.slider);
        }
        if (caps.wheel && (axes.changed & TabletAxes::wheel_axis) != 0) {
            self.tool_wheel(tool.id, axes.wheel_degrees, axes.wheel_clicks);
        }
        self.tool_frame(tool.id, time);
    };

    std::visit(
        [&](const auto& e) {
            using E = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<E, TabletDeviceAdded>) {
                if (e.is_pad) {
                    if (!fed_pads.contains(e.device)) {
                        fed_pads[e.device] = self.add_pad(TabletPadInfo{e.path, e.pad});
                    }
                } else if (!fed_tablets.contains(e.device)) {
                    fed_tablets[e.device] =
                        self.add_tablet(TabletInfo{e.name, e.vid, e.pid, e.path, e.bustype});
                }
            } else if constexpr (std::is_same_v<E, TabletDeviceRemoved>) {
                if (auto it = fed_tablets.find(e.device); it != fed_tablets.end()) {
                    const TabletId gone = it->second;
                    for (auto& [backend, tool] : fed_tools) {
                        if (tool.tablet == gone) {
                            tool.in_range = false;
                        }
                    }
                    self.remove_tablet(gone);
                    fed_tablets.erase(it);
                } else if (auto pit = fed_pads.find(e.device); pit != fed_pads.end()) {
                    self.remove_pad(pit->second);
                    fed_pads.erase(pit);
                    std::erase_if(fed_modes, [&](const auto& m) { return m.first.first == e.device; });
                }
            } else if constexpr (std::is_same_v<E, TabletToolProximityEvent>) {
                auto tablet = fed_tablets.find(e.device);
                if (tablet == fed_tablets.end()) {
                    return;
                }
                FedTool& tool = fed_tools[e.tool];
                if (tool.id == 0) {
                    tool.id = self.add_tool(TabletToolInfo{e.type, e.serial, e.wacom_id, e.caps});
                }
                tool.tablet = tablet->second;
                if (!e.in) {
                    tool.in_range = false;
                    if (tool.down) {
                        self.tool_up(tool.id);
                        tool.down = false;
                    }
                    if (self.tool_focus(tool.id).valid()) {
                        self.tool_proximity_out(tool.id);
                        sync_pads(tool.tablet);
                    }
                    return;
                }
                tool.in_range = true;
                const auto target = retarget(tool, e.axes);
                send_axes(tool, e.axes, target, true, e.time_ms);
            } else if constexpr (std::is_same_v<E, TabletToolAxisEvent>) {
                auto it = fed_tools.find(e.tool);
                if (it == fed_tools.end() || !it->second.in_range) {
                    return;
                }
                FedTool& tool = it->second;
                const bool was = self.tool_focus(tool.id).valid();
                const auto target = retarget(tool, e.axes);
                send_axes(tool, e.axes, target, !was, e.time_ms);
            } else if constexpr (std::is_same_v<E, TabletToolTipEvent>) {
                auto it = fed_tools.find(e.tool);
                if (it == fed_tools.end() || !it->second.in_range) {
                    return;
                }
                FedTool& tool = it->second;
                const auto target = retarget(tool, e.axes);
                if (!target.has_value()) {
                    return;
                }
                // Motion first, so the client sees where the tip came down.
                send_axes(tool, e.axes, target, false, e.time_ms);
                if (e.down && !tool.down) {
                    self.tool_down(tool.id);
                } else if (!e.down && tool.down) {
                    self.tool_up(tool.id);
                }
                tool.down = e.down;
                self.tool_frame(tool.id, e.time_ms);
            } else if constexpr (std::is_same_v<E, TabletToolButtonEvent>) {
                auto it = fed_tools.find(e.tool);
                if (it == fed_tools.end() || !it->second.in_range) {
                    return;
                }
                self.tool_button(it->second.id, e.button, e.pressed);
                self.tool_frame(it->second.id, e.time_ms);
            } else if constexpr (std::is_same_v<E, TabletPadButtonEvent>) {
                auto it = fed_pads.find(e.device);
                if (it == fed_pads.end()) {
                    return;
                }
                // A mode change is announced before the button that caused it is
                // used, so a client switching its UI sees the new mode first.
                auto& mode = fed_modes[{e.device, e.group}];
                if (mode != e.mode) {
                    mode = e.mode;
                    self.pad_mode_switch(it->second, e.group, e.mode, e.time_ms);
                }
                self.pad_button(it->second, e.time_ms, e.button, e.pressed);
            } else if constexpr (std::is_same_v<E, TabletPadRingEvent>) {
                if (auto it = fed_pads.find(e.device); it != fed_pads.end()) {
                    self.pad_ring(it->second, e.ring, e.angle, e.time_ms);
                }
            } else if constexpr (std::is_same_v<E, TabletPadStripEvent>) {
                if (auto it = fed_pads.find(e.device); it != fed_pads.end()) {
                    self.pad_strip(it->second, e.strip, e.position, e.time_ms);
                }
            }
        },
        event);
}

} // namespace luminaria
