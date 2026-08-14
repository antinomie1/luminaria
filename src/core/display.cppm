// luminaria/core/display.cppm — owning RAII wrapper over wl_display.
//
// This is the root object: it owns the wl_display and its event loop, hosts the
// Wayland socket, and runs the main loop. Move-only; the C handle is destroyed
// exactly once, in the destructor. Importing luminaria pulls in no libwayland headers (the
// wl_display type is opaque here; all libwayland calls live in display.cpp).

module;

#include "detail/wayland_fwd.h"
#include <string>

#include <wayland-server-core.h>

export module luminaria:display;

import :event_loop;
import :expected;

export namespace luminaria {

class Display {
    wl_display* display_ = nullptr; // owned

    explicit Display(wl_display* display) noexcept : display_(display) {}

public:
    /// Create a fresh display + event loop. Fails only on allocation failure.
    [[nodiscard]] static Result<Display> create();

    ~Display();
    Display(Display&& o) noexcept : display_(o.display_) { o.display_ = nullptr; }
    Display& operator=(Display&& o) noexcept;
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
    [[nodiscard]] wl_display* c_ptr() const noexcept { return display_; }
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

Display::~Display() {
    if (display_ != nullptr) {
        wl_display_destroy(display_);
    }
}

Display& Display::operator=(Display&& o) noexcept {
    if (this != &o) {
        if (display_ != nullptr) {
            wl_display_destroy(display_);
        }
        display_ = o.display_;
        o.display_ = nullptr;
    }
    return *this;
}

EventLoop Display::event_loop() const {
    return EventLoop{wl_display_get_event_loop(display_)};
}

Result<std::string> Display::add_socket_auto() {
    const char* name = wl_display_add_socket_auto(display_);
    if (name == nullptr) {
        return fail("wl_display_add_socket_auto() failed");
    }
    return std::string{name};
}

Status Display::init_shm() {
    if (wl_display_init_shm(display_) != 0) {
        return fail("wl_display_init_shm() failed");
    }
    return ok();
}

void Display::run() {
    wl_display_run(display_);
}

void Display::terminate() {
    wl_display_terminate(display_);
}

} // namespace luminaria
