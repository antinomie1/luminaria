// luminaria/backend/wayland.hpp — nested backend: run as a client inside a parent
// Wayland compositor. Each output is a parent window (xdg_toplevel); frames are
// driven by the parent's frame callbacks; content is presented via wl_shm.
//
// Requires a running parent compositor (WAYLAND_DISPLAY). TODO: wl_shm CPU
// present + per-frame buffer alloc; upgrade to linux-dmabuf zero-copy if it matters.
#pragma once

#include <memory>

#include "luminaria/backend.hpp"
#include "luminaria/core/event_loop.hpp"
#include "luminaria/core/expected.hpp"
#include "luminaria/output.hpp"

namespace luminaria {

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

    /// Create a parent window of the given size. Call before start().
    Output& add_output(int width, int height);

    Status start() override;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit WaylandBackend(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
