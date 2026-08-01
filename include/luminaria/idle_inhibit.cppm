// luminaria/idle_inhibit.cppm — zwp_idle_inhibit_manager_v1: "don't blank the
// screen, I'm playing a video".
//
// A client creates an inhibitor for one of its surfaces and holds it for as
// long as the screen must stay awake. There is no wire traffic back: the
// protocol is entirely a request, and honouring it is the compositor's job.
//
// The one rule worth respecting is that an inhibitor only counts while its
// surface is actually VISIBLE — a paused-and-minimised video player must not
// keep the display on forever. This library cannot know what is visible, so it
// gives you the inhibitors and a hook: set `visible` on each one from your own
// layout, and `inhibited()` counts only those. It defaults to true, which is
// the right answer for a compositor that shows everything it maps.

module;

#include <cstddef>
#include <memory>

export module luminaria:idle_inhibit;

import :core.expected;
import :core.signal;

export namespace luminaria {

class Display;
class Surface;
class IdleInhibitor;

struct NewIdleInhibitor {
    IdleInhibitor& inhibitor;
};
struct IdleInhibitorDestroy {
    IdleInhibitor& inhibitor;
};
/// The effective answer to "may the screen blank?" flipped. Fires only on the
/// transition, so wiring it straight to a screen-blanking timer is enough.
struct IdleInhibitChange {
    bool inhibited;
};

/// One zwp_idle_inhibitor_v1. Owned by its resource; address stable for its
/// lifetime, so signals may capture `IdleInhibitor&`.
class IdleInhibitor {
public:
    virtual ~IdleInhibitor() = default;
    IdleInhibitor(const IdleInhibitor&) = delete;
    IdleInhibitor& operator=(const IdleInhibitor&) = delete;

    Signal<IdleInhibitorDestroy> destroy;

    /// The surface the client wants kept awake. Never null: an inhibitor whose
    /// surface dies destroys itself first.
    [[nodiscard]] virtual Surface& surface() noexcept = 0;

    /// Whether this inhibitor currently counts. Set it false when the surface is
    /// off-screen, minimised or on another workspace; the manager recomputes
    /// `inhibited()` and fires `changed` if the answer flipped.
    [[nodiscard]] virtual bool visible() const noexcept = 0;
    virtual void set_visible(bool visible) = 0;

protected:
    IdleInhibitor() = default;
};

/// The zwp_idle_inhibit_manager_v1 global (version 1). Move-only;
/// pointer-stable state.
class IdleInhibitManager {
public:
    [[nodiscard]] static Result<IdleInhibitManager> create(Display& display);

    ~IdleInhibitManager();
    IdleInhibitManager(IdleInhibitManager&&) noexcept;
    IdleInhibitManager& operator=(IdleInhibitManager&&) noexcept;
    IdleInhibitManager(const IdleInhibitManager&) = delete;
    IdleInhibitManager& operator=(const IdleInhibitManager&) = delete;

    [[nodiscard]] Signal<NewIdleInhibitor>& new_inhibitor() noexcept;
    /// Fires when `inhibited()` changes value.
    [[nodiscard]] Signal<IdleInhibitChange>& changed() noexcept;

    /// True while at least one visible inhibitor is held: do not blank.
    [[nodiscard]] bool inhibited() const noexcept;
    /// Every live inhibitor, visible or not.
    [[nodiscard]] std::size_t inhibitor_count() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit IdleInhibitManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
