// luminaria/relative_pointer.cppm — zwp_relative_pointer_manager_v1.
//
// wl_pointer reports where the cursor IS; this reports how far it MOVED. A game
// or a 3D viewport that has hidden the cursor and locked it in place gets no
// wl_pointer.motion at all — the pointer is not going anywhere — so without this
// protocol it cannot see the mouse move. Relative motion is delivered on top of
// wl_pointer, never instead of it, and it comes in two flavours: `dx`/`dy` with
// the pointer-acceleration curve applied (what the desktop uses), and
// `dx_unaccel`/`dy_unaccel` straight off the device (what a game wants).
//
// The manager routes to whoever holds the seat's POINTER FOCUS, so keep the
// Seat alive at least as long as this. The compositor pumps it from its
// backend's motion handler, next to the absolute-position bookkeeping:
//
//     relative->send_motion(time_us, dx, dy, dx_unaccel, dy_unaccel);
//
// libinput hands you both pairs already; a backend that only has one should
// pass the same values twice rather than inventing an acceleration curve.

module;

#include <cstdint>
#include <memory>

export module luminaria:relative_pointer;

import :core.expected;

export namespace luminaria {

class Display;
class Seat;

/// The zwp_relative_pointer_manager_v1 global (version 1). Move-only;
/// pointer-stable state.
class RelativePointerManager {
public:
    /// Create the global. Events go to `seat`'s pointer-focused client.
    [[nodiscard]] static Result<RelativePointerManager> create(Display& display, Seat& seat);

    ~RelativePointerManager();
    RelativePointerManager(RelativePointerManager&&) noexcept;
    RelativePointerManager& operator=(RelativePointerManager&&) noexcept;
    RelativePointerManager(const RelativePointerManager&) = delete;
    RelativePointerManager& operator=(const RelativePointerManager&) = delete;

    /// Deliver relative motion to the pointer-focused client. `time_us` is a
    /// microsecond timestamp from the same monotonic clock as the rest of the
    /// input events. Deltas are in surface-local units. A client that never
    /// bound a relative pointer sees nothing; so does one without pointer focus.
    void send_motion(std::uint64_t time_us, double dx, double dy, double dx_unaccel,
                     double dy_unaccel);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit RelativePointerManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
