// luminaria/core/display.cppm — owning RAII wrapper over wl_display.
//
// This is the root object: it owns the wl_display and its event loop, hosts the
// Wayland socket, and runs the main loop. Move-only; the C handle lives in a
// CUnique, so it is destroyed exactly once and this class needs no destructor
// of its own. Importing luminaria pulls in no libwayland headers — the
// wl_display type is opaque here, and every libwayland call is below the
// implementation divider.

module;

#include "detail/wayland_fwd.h"
#include <string>

#include <wayland-server-core.h>

export module luminaria:display;

import :event_loop;
import :expected;
import :handle;

export namespace luminaria {

class Display {
    CUnique<wl_display, wl_display_destroy> display_;

    explicit Display(wl_display* display) noexcept : display_(display) {}

public:
    /// Create a fresh display + event loop. Fails only on allocation failure.
    [[nodiscard]] static Result<Display> create();

    // Rule of zero: the handle destroys itself exactly once, so move is just a
    // pointer steal and there is nothing for a destructor to do.
    ~Display() = default;
    Display(Display&&) noexcept = default;
    Display& operator=(Display&&) noexcept = default;
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    /// The event loop driving this display (non-owning view).
    [[nodiscard]] EventLoop event_loop() const;

    /// Bind an auto-named Wayland socket (WAYLAND_DISPLAY-style name), returning it.
    [[nodiscard]] Result<std::string> add_socket_auto();

    /// Enable libwayland's built-in wl_shm global (shared-memory client buffers).
    [[nodiscard]] Status init_shm();

    /// Run the main loop until terminate() is called.
    void run();

    /// Ask run() to return at the next opportunity.
    void terminate();

    /// Escape hatch for code that still needs the raw handle (backends, protocols).
    [[nodiscard]] wl_display* c_ptr() const noexcept { return display_.get(); }
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

Result<Display> Display::create() {
    wl_display* d = wl_display_create();
    if (d == nullptr) {
        return fail("wl_display_create() failed");
    }
    return Display{d};
}

EventLoop Display::event_loop() const {
    return EventLoop{wl_display_get_event_loop(display_.get())};
}

Result<std::string> Display::add_socket_auto() {
    const char* name = wl_display_add_socket_auto(display_.get());
    if (name == nullptr) {
        return fail("wl_display_add_socket_auto() failed");
    }
    return std::string{name};
}

Status Display::init_shm() {
    if (wl_display_init_shm(display_.get()) != 0) {
        return fail("wl_display_init_shm() failed");
    }
    return ok();
}

void Display::run() {
    wl_display_run(display_.get());
}

void Display::terminate() {
    wl_display_terminate(display_.get());
}

} // namespace luminaria
