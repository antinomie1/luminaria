// luminaria/core/display.cppm — owning RAII wrapper over wl_display.
//
// This is the root object: it owns the wl_display and its event loop, hosts the
// Wayland socket, and runs the main loop. Move-only; the C handle is destroyed
// exactly once, in the destructor. Importing luminaria pulls in no libwayland headers (the
// wl_display type is opaque here; all libwayland calls live in display.cpp).

module;

#include "luminaria/detail/wayland_fwd.h"

#include <string>

export module luminaria:core.display;

import :core.event_loop;
import :core.expected;

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
