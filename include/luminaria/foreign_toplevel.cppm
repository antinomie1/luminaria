// luminaria/foreign_toplevel.cppm — zwlr_foreign_toplevel_management_v1.
//
// The protocol a taskbar, dock or window switcher speaks: it lists every window
// on the desktop with its title, app id, state and output, and asks to
// activate / minimize / maximize / close them.
//
// Unlike wlroots, nothing has to be published by hand. `track(shell)` mirrors an
// XdgShell: a window appears in the list when it maps, its title and state
// updates follow the toplevel's own signals, and it disappears when it unmaps or
// dies. What comes back is `request()` — a taskbar asking for something, which
// the compositor grants by calling the usual Toplevel methods (or ignores).
//
// Note that this protocol hands one client the ability to enumerate and control
// every other client's windows. It is meant for the desktop's own components; a
// compositor that sandboxes applications should not expose the global to them.

module;

#include "luminaria/detail/wayland_fwd.h"

#include <memory>

export module luminaria:foreign_toplevel;

import :core.expected;
import :core.signal;

export namespace luminaria {

class Display;
class OutputGlobal;
class Toplevel;
class XdgShell;

/// A window-list client asked for something. Nothing happens until the
/// compositor acts on it — this is a request, not a command.
struct ForeignToplevelRequest {
    enum class Kind {
        maximize,
        unmaximize,
        minimize,
        unminimize,
        fullscreen,
        unfullscreen,
        activate,
        close,
    };
    Toplevel& toplevel;
    Kind kind;
    /// The wl_seat the client acted on (`activate` only), else null.
    wl_resource* seat = nullptr;
    /// The wl_output the client wants (`fullscreen` only, may be null for
    /// "compositor's choice").
    wl_resource* output = nullptr;
};

/// The zwlr_foreign_toplevel_manager_v1 global (version 3). Move-only;
/// pointer-stable state.
class ForeignToplevelManager {
public:
    [[nodiscard]] static Result<ForeignToplevelManager> create(Display& display);

    ~ForeignToplevelManager();
    ForeignToplevelManager(ForeignToplevelManager&&) noexcept;
    ForeignToplevelManager& operator=(ForeignToplevelManager&&) noexcept;
    ForeignToplevelManager(const ForeignToplevelManager&) = delete;
    ForeignToplevelManager& operator=(const ForeignToplevelManager&) = delete;

    /// Publish every window of `shell` to window-list clients, and keep the list
    /// current by itself. Call it once, right after creating the shell; `shell`
    /// must outlive this manager.
    void track(XdgShell& shell);

    /// Publish one window that `track()` cannot see (an Xwayland window, say).
    /// Same lifecycle rules: it appears on map and vanishes on unmap/destroy.
    void add(Toplevel& toplevel);

    /// Tell clients which output this window is on. A taskbar per monitor shows
    /// only its own windows, so without this every window shows up everywhere.
    /// Null removes the association.
    void set_output(Toplevel& toplevel, OutputGlobal* output);

    /// Fires for each client request.
    [[nodiscard]] Signal<ForeignToplevelRequest>& request() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit ForeignToplevelManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
