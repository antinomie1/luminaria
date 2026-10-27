// luminaria/cursor_shape.cppm — the wp_cursor_shape_manager_v1 global.
//
// Instead of every client loading a cursor theme, allocating a buffer and
// calling wl_pointer.set_cursor, it names the shape it wants ("text",
// "ns-resize", …) and the compositor draws it. One theme, one scale, one place
// that gets HiDPI right.
//
// The compositor subscribes to `request()` and paints the named cursor; if it
// ignores the signal the cursor simply doesn't change.
//
// Importing luminaria pulls in no libwayland headers: the C types this
// interface names are forward-declared in the global module fragment.

module;

#include "detail/wayland_fwd.h"
#include <cstdint>

#include <wayland-server-core.h>
#include "cursor-shape-v1-protocol.h"

export module luminaria:cursor_shape;

import std;

import :display;
import :expected;
import :protocol_helper;
import :signal;

export namespace luminaria {

class Display;

/// "This pointer should show `name`." `shape` is the raw protocol enum value;
/// `name` is the matching XDG/CSS cursor name ("default", "not-allowed", …),
/// which is what a cursor theme is indexed by. `name` is a static string.
struct CursorShapeRequest {
    wl_resource* pointer; // the wl_pointer this applies to; null for a tablet tool
    std::uint32_t serial; // the enter serial the client is answering
    std::uint32_t shape;
    const char* name;
    /// The zwp_tablet_tool_v2 this applies to, null for a pointer.
    /// `TabletManager::tool_id_of()` says which tool it is.
    wl_resource* tablet_tool = nullptr;
};

/// The wp_cursor_shape_manager_v1 global (version 2). Move-only; pointer-stable
/// state so the libwayland global can hold a pointer to it.
class CursorShapeManager {
public:
    [[nodiscard]] static Result<CursorShapeManager> create(Display& display);

    ~CursorShapeManager();
    CursorShapeManager(CursorShapeManager&&) noexcept;
    CursorShapeManager& operator=(CursorShapeManager&&) noexcept;
    CursorShapeManager(const CursorShapeManager&) = delete;
    CursorShapeManager& operator=(const CursorShapeManager&) = delete;

    /// Fires when a client asks for a named cursor.
    [[nodiscard]] Signal<CursorShapeRequest>& request() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit CursorShapeManager(std::unique_ptr<Impl> impl) noexcept;
};

/// XDG/CSS cursor name for a wp_cursor_shape_device_v1 shape value, or null if
/// the value is not a shape we know.
[[nodiscard]] const char* cursor_shape_name(std::uint32_t shape) noexcept;

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Implements wp_cursor_shape_manager_v1 (version 2). The device object is a
// thin handle: it remembers which wl_pointer (or tablet tool) it decorates and
// forwards each set_shape to the compositor as a named cursor.

namespace luminaria {

namespace {

// Protocol shape values are 1-based and contiguous; the names are the XDG
// cursor-spec ones, which is what a theme lookup wants.
constexpr std::array<const char*, 36> kShapeNames{
    "default",     "context-menu", "help",        "pointer",     "progress",
    "wait",        "cell",         "crosshair",   "text",        "vertical-text",
    "alias",       "copy",         "move",        "no-drop",     "not-allowed",
    "grab",        "grabbing",     "e-resize",    "n-resize",    "ne-resize",
    "nw-resize",   "s-resize",     "se-resize",   "sw-resize",   "w-resize",
    "ew-resize",   "ns-resize",    "nesw-resize", "nwse-resize", "col-resize",
    "row-resize",  "all-scroll",   "zoom-in",     "zoom-out",    "dnd-ask",
    "all-resize"};

struct ShapeDevice {
    Signal<CursorShapeRequest>* request = nullptr;
    wl_resource* pointer = nullptr;     // null for a tablet tool
    wl_resource* tablet_tool = nullptr; // null for a pointer, and once the tool is destroyed
    bool for_tool = false;              // made by get_tablet_tool_v2
    wl_listener tool_destroyed{};

    ShapeDevice(Signal<CursorShapeRequest>* request, wl_resource* pointer, wl_resource* tool)
        : request(request), pointer(pointer), tablet_tool(tool), for_tool(tool != nullptr) {
        if (tool != nullptr) {
            // The client owns the tool object and may destroy it first; the
            // device must not name a resource that is gone.
            tool_destroyed.notify = [](wl_listener* l, void*) {
                auto* self = wl_container_of(l, static_cast<ShapeDevice*>(nullptr), tool_destroyed);
                self->tablet_tool = nullptr;
                wl_list_remove(&self->tool_destroyed.link);
                wl_list_init(&self->tool_destroyed.link);
            };
            wl_resource_add_destroy_listener(tool, &tool_destroyed);
        }
    }
    ~ShapeDevice() {
        if (tool_destroyed.notify != nullptr) {
            wl_list_remove(&tool_destroyed.link);
        }
    }
    ShapeDevice(const ShapeDevice&) = delete;
    ShapeDevice& operator=(const ShapeDevice&) = delete;
};

void device_set_shape(wl_client* client, wl_resource* resource, uint32_t serial, uint32_t shape) {
    const char* name = cursor_shape_name(shape);
    if (name == nullptr) {
        wl_resource_post_error(resource, WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
                               "unknown cursor shape %u", shape);
        return;
    }
    (void)client;
    auto* device = static_cast<ShapeDevice*>(wl_resource_get_user_data(resource));
    if (device->for_tool && device->tablet_tool == nullptr) {
        return; // the tool is gone; there is nothing to give a shape to
    }
    CursorShapeRequest event{device->pointer, serial, shape, name, device->tablet_tool};
    device->request->emit(event);
}

constexpr struct wp_cursor_shape_device_v1_interface device_impl = {
    .destroy = resource_destroy_request,
    .set_shape = device_set_shape,
};

void make_device(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* pointer,
                 wl_resource* tablet_tool = nullptr) {
    auto* signal = static_cast<Signal<CursorShapeRequest>*>(wl_resource_get_user_data(manager));
    auto device = std::make_unique<ShapeDevice>(signal, pointer, tablet_tool);
    create_user_resource<ShapeDevice, &wp_cursor_shape_device_v1_interface, &device_impl>(
        client, wl_resource_get_version(manager), id, std::move(device), manager);
}

void manager_get_pointer(wl_client* client, wl_resource* manager, uint32_t id,
                         wl_resource* pointer) {
    make_device(client, manager, id, pointer);
}

// The request names the tool it is for; the compositor resolves it with
// `TabletManager::tool_id_of()` while handling the signal. The device watches
// the tool resource, so a request after the tool's destruction is dropped.
void manager_get_tablet_tool_v2(wl_client* client, wl_resource* manager, uint32_t id,
                                wl_resource* tablet_tool) {
    make_device(client, manager, id, nullptr, tablet_tool);
}

constexpr struct wp_cursor_shape_manager_v1_interface manager_impl = {
    .destroy = resource_destroy_request,
    .get_pointer = manager_get_pointer,
    .get_tablet_tool_v2 = manager_get_tablet_tool_v2,
};

} // namespace

const char* cursor_shape_name(std::uint32_t shape) noexcept {
    if (shape == 0 || shape > kShapeNames.size()) {
        return nullptr;
    }
    return kShapeNames[shape - 1];
}

struct CursorShapeManager::Impl {
    // First member: the wl_global's user_data points here, and the device glue
    // reads the signal straight off that pointer.
    Signal<CursorShapeRequest> request;
    WlGlobal global;
};

CursorShapeManager::CursorShapeManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
CursorShapeManager::~CursorShapeManager() = default;
CursorShapeManager::CursorShapeManager(CursorShapeManager&&) noexcept = default;
CursorShapeManager& CursorShapeManager::operator=(CursorShapeManager&&) noexcept = default;

Signal<CursorShapeRequest>& CursorShapeManager::request() noexcept { return impl_->request; }

Result<CursorShapeManager> CursorShapeManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    auto global = create_wl_global<&wp_cursor_shape_manager_v1_interface,
                                   default_bind<&wp_cursor_shape_manager_v1_interface,
                                                &manager_impl>>(display, 2, &impl->request);
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return CursorShapeManager{std::move(impl)};
}

} // namespace luminaria
