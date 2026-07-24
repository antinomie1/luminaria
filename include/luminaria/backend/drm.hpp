// luminaria/backend/drm.hpp — bare-metal KMS backend. Drives a real monitor via
// /dev/dri/card* using dumb buffers (software fill) and page-flipping, with
// frames paced by the DRM vblank.
//
// Requires DRM master, i.e. run from a VT with no other compositor holding the
// GPU. TODO: dumb buffers + no libseat — no VT-switch/resume handling, no
// GBM/Vulkan scanout; upgrade to GBM + Vulkan + libseat for a real session.
#pragma once

#include <memory>
#include <string>

#include "luminaria/backend.hpp"
#include "luminaria/core/event_loop.hpp"
#include "luminaria/core/expected.hpp"

namespace luminaria {

class DrmBackend final : public Backend {
public:
    /// Auto-detect: try each /dev/dri/card* and use the first we can master with
    /// a connected output. Fails (so callers can skip) if none works.
    [[nodiscard]] static Result<DrmBackend> create(EventLoop loop);

    /// Open a specific device, become DRM master, and pick the first connected
    /// output. Fails if it can't be opened/mastered or has no output.
    [[nodiscard]] static Result<DrmBackend> create(EventLoop loop, std::string device);

    ~DrmBackend();
    DrmBackend(DrmBackend&&) noexcept;
    DrmBackend& operator=(DrmBackend&&) noexcept;
    DrmBackend(const DrmBackend&) = delete;
    DrmBackend& operator=(const DrmBackend&) = delete;

    Status start() override;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit DrmBackend(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
