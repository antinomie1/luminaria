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

#include "luminaria/detail/wayland_fwd.h"

#include <cstdint>
#include <memory>

export module luminaria:cursor_shape;

import :core.expected;
import :core.signal;

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
