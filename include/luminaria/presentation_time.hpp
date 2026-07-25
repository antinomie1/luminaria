// luminaria/presentation_time.hpp — the wp_presentation global.
//
// A client that animates needs to know *when* its frame reached the screen and
// how long a refresh lasts; without it, motion is timed against guesses and
// judders. The compositor drives this from the backend's `Output::present`
// signal, which carries the real vblank timestamp.
//
// Public header stays C-header-free.
#pragma once

#include <memory>

#include "luminaria/core/expected.hpp"
#include "luminaria/output.hpp"

struct wl_resource;

namespace luminaria {

class Display;
class Surface;

/// The wp_presentation global (version 2). Move-only; pointer-stable state so
/// the libwayland global can hold a pointer to it.
class Presentation {
public:
    [[nodiscard]] static Result<Presentation> create(Display& display);

    ~Presentation();
    Presentation(Presentation&&) noexcept;
    Presentation& operator=(Presentation&&) noexcept;
    Presentation(const Presentation&) = delete;
    Presentation& operator=(const Presentation&) = delete;

    /// Everything committed to `surface` before the presented frame is now on
    /// screen. `sync_output` is the client's wl_output resource for the display
    /// that showed it, or null to skip the sync_output event.
    void notify_presented(Surface& surface, const PresentEvent& event,
                          wl_resource* sync_output = nullptr);

    /// The content never made it (frame dropped, surface unmapped). Clients wait
    /// forever otherwise.
    void notify_discarded(Surface& surface);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit Presentation(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
