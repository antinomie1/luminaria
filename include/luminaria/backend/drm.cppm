// luminaria/backend/drm.cppm — bare-metal KMS backend. Drives a real monitor via
// /dev/dri/card* with **atomic** modesetting, frames paced by the DRM vblank.
//
// The scanout path is `Output::import_scanout()` + `commit_scanout()`: hand it a
// dmabuf the Vulkan renderer rendered into and it becomes a KMS framebuffer, so
// a frame goes from client buffer to screen without touching the CPU. Dumb
// buffers remain only for `commit(Color)` / `commit_frame(pixels)`.
//
// Every connected connector becomes an Output, and a udev monitor keeps that set
// live: plug a monitor in and `new_output` fires, unplug it and `Output::destroy`
// does.
//
// Requires DRM master, i.e. run from a VT with no other compositor holding the
// GPU. Pass a luminaria::Session and devices are opened through libseat, which
// is what makes VT switching safe: master is dropped on the way out and the
// modeset re-applied on the way back in.
//
// Each output drives a primary plane and, when the hardware has one to spare, a
// cursor plane — so moving the pointer costs one small atomic commit instead of
// a repaint. TODO: no direct scanout of a client buffer (fullscreen bypass),
// and no mode switching: each output uses its connector's preferred mode.

module;

#include <memory>
#include <string>

export module luminaria:backend.drm;

import :backend;
import :core.event_loop;
import :core.expected;

export namespace luminaria {

class Session;

class DrmBackend final : public Backend {
public:
    /// Auto-detect: try each /dev/dri/card* and use the first we can master with
    /// a connected output. Fails (so callers can skip) if none works.
    ///
    /// Pass a `Session` (see luminaria/session.cppm) to open the card through
    /// libseat: the backend then drops DRM master when the VT is switched away
    /// and re-applies the modeset when it comes back. Without one the card is
    /// opened directly and VT switching corrupts the display.
    [[nodiscard]] static Result<DrmBackend> create(EventLoop loop, Session* session = nullptr);

    /// Open a specific device, become DRM master, and pick the first connected
    /// output. Fails if it can't be opened/mastered or has no output.
    [[nodiscard]] static Result<DrmBackend> create(EventLoop loop, std::string device,
                                                   Session* session = nullptr);

    ~DrmBackend();
    DrmBackend(DrmBackend&&) noexcept;
    DrmBackend& operator=(DrmBackend&&) noexcept;
    DrmBackend(const DrmBackend&) = delete;
    DrmBackend& operator=(const DrmBackend&) = delete;

    Status start() override;

    /// Re-read the connector list and diff it against the outputs we have:
    /// new monitors get an Output (and `new_output`), unplugged ones are
    /// destroyed (and `Output::destroy` fires). Called automatically on udev
    /// hotplug events once start() has run; exposed for a manual poke.
    void scan_connectors();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit DrmBackend(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
