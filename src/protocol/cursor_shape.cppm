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
import :signal;

export namespace luminaria {

class Display;

/// "This pointer should show `name`." `shape` is the raw protocol enum value;
/// `name` is the matching XDG/CSS cursor name ("default", "not-allowed", …),
/// which is what a cursor theme is indexed by. `name` is a static string.
struct CursorShapeRequest {
    wl_resource* pointer; // the wl_pointer this applies to
    std::uint32_t serial; // the enter serial the client is answering
    std::uint32_t shape;
    const char* name;
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
    wl_resource* pointer = nullptr; // null for a tablet tool
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
    CursorShapeRequest event{device->pointer, serial, shape, name};
    device->request->emit(event);
}

void device_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void device_resource_destroy(wl_resource* resource) {
    delete static_cast<ShapeDevice*>(wl_resource_get_user_data(resource));
}

constexpr struct wp_cursor_shape_device_v1_interface device_impl = {
    .destroy = device_destroy_request,
    .set_shape = device_set_shape,
};

void make_device(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* pointer);

void manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void manager_get_pointer(wl_client* client, wl_resource* manager, uint32_t id,
                         wl_resource* pointer) {
    make_device(client, manager, id, pointer);
}

// We ship no tablet support, but the object still has to exist or the client
// dies on a null slot the moment it asks.
void manager_get_tablet_tool_v2(wl_client* client, wl_resource* manager, uint32_t id,
                                wl_resource* /*tablet_tool*/) {
    make_device(client, manager, id, nullptr);
}

constexpr struct wp_cursor_shape_manager_v1_interface manager_impl = {
    .destroy = manager_destroy_request,
    .get_pointer = manager_get_pointer,
    .get_tablet_tool_v2 = manager_get_tablet_tool_v2,
};

void make_device(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* pointer) {
    wl_resource* resource = wl_resource_create(client, &wp_cursor_shape_device_v1_interface,
                                               wl_resource_get_version(manager),
                                               static_cast<int>(id));
    if (resource == nullptr) {
        wl_resource_post_no_memory(manager);
        return;
    }
    auto* signal = static_cast<Signal<CursorShapeRequest>*>(wl_resource_get_user_data(manager));
    wl_resource_set_implementation(resource, &device_impl, new ShapeDevice{signal, pointer},
                                   device_resource_destroy);
}

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wp_cursor_shape_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

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
    wl_global* global = nullptr;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

CursorShapeManager::CursorShapeManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
CursorShapeManager::~CursorShapeManager() = default;
CursorShapeManager::CursorShapeManager(CursorShapeManager&&) noexcept = default;
CursorShapeManager& CursorShapeManager::operator=(CursorShapeManager&&) noexcept = default;

Signal<CursorShapeRequest>& CursorShapeManager::request() noexcept { return impl_->request; }

Result<CursorShapeManager> CursorShapeManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->global = wl_global_create(display.c_ptr(), &wp_cursor_shape_manager_v1_interface, 2,
                                    &impl->request, manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wp_cursor_shape_manager_v1) failed");
    }
    return CursorShapeManager{std::move(impl)};
}

} // namespace luminaria
