// luminaria/backend/wayland.hpp — nested backend: run as a client inside a parent
// Wayland compositor. Each output is a parent window (xdg_toplevel); frames are
// driven by the parent's frame callbacks; content is presented via wl_shm.
//
// Requires a running parent compositor (WAYLAND_DISPLAY). TODO: wl_shm CPU
// present + per-frame buffer alloc; upgrade to linux-dmabuf zero-copy if it matters.
#pragma once

#include <memory>
#include <string>

#include "luminaria/backend.hpp"
#include "luminaria/core/event_loop.hpp"
#include "luminaria/core/expected.hpp"
#include "luminaria/core/signal.hpp"
#include "luminaria/input_event.hpp"
#include "luminaria/output.hpp"

namespace luminaria {

/// Who draws the frame around OUR nested window, as negotiated with the parent
/// compositor via xdg-decoration-unstable-v1. The mirror image of
/// luminaria::DecorationMode in <luminaria/xdg_decoration.hpp>, which is the
/// same question asked by our own clients.
/// "The parent compositor's keyboard layout is this." `text` is an xkb keymap
/// in text form, borrowed for the duration of the emit.
struct KeymapChange {
    const std::string& text;
};

enum class HostDecorationMode {
    None,       ///< the parent has no xdg-decoration global: no frame at all
    ClientSide, ///< the parent insists WE draw it (GTK-style CSD); we don't
    ServerSide, ///< the parent draws a native titlebar around us — what we ask for
};

class WaylandBackend final : public Backend {
public:
    /// Connect to the parent compositor and bind its globals. Fails if there is
    /// no parent (WAYLAND_DISPLAY) or it lacks wl_compositor/xdg_wm_base/wl_shm.
    [[nodiscard]] static Result<WaylandBackend> create(EventLoop loop);

    ~WaylandBackend();
    WaylandBackend(WaylandBackend&&) noexcept;
    WaylandBackend& operator=(WaylandBackend&&) noexcept;
    WaylandBackend(const WaylandBackend&) = delete;
    WaylandBackend& operator=(const WaylandBackend&) = delete;

    /// Create a parent window of the given size, titled `title`. The window
    /// asks the parent compositor for a native (server-side) decoration, so it
    /// looks like any other application on the host desktop. Call before start().
    Output& add_output(int width, int height, std::string title = "luminaria");

    Status start() override;

    /// What the parent granted. Only meaningful after start() — the negotiation
    /// completes with the first configure. `None` means the parent doesn't
    /// implement xdg-decoration at all (e.g. GNOME/Mutter), so the nested window
    /// is bare; `ClientSide` means it refused to decorate us.
    [[nodiscard]] HostDecorationMode decoration_mode() const noexcept;

    // Input forwarded from the parent compositor (cursor is over our window).
    // Coordinates are output-local; the compositor hit-tests and routes to a
    // client surface. Pointer leave clears focus (motion x/y set to -1,-1).
    Signal<KeyEvent> key;
    Signal<ModifiersEvent> modifiers;
    /// The parent compositor told us its keyboard layout. Hand `text` to
    /// `Seat::set_keymap()` so our clients see the same layout the user is
    /// actually typing on — otherwise the modifier masks forwarded above are
    /// interpreted against a different keymap and non-US layouts misbehave.
    Signal<KeymapChange> keymap_changed;

    /// The parent's xkb keymap in text form; empty until it sends one.
    [[nodiscard]] const std::string& keymap() const noexcept;
    Signal<PointerMotionAbsEvent> pointer_motion;
    Signal<PointerButtonEvent> pointer_button;
    /// One scroll frame, accumulated across the parent's axis events.
    Signal<PointerAxisEvent> pointer_axis;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit WaylandBackend(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
