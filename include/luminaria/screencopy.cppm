// luminaria/screencopy.cppm — screen capture protocol (wlr-screencopy-unstable-v1 +
// ext-image-copy-capture-v1). Lets clients capture outputs into wl_shm buffers.
//
// RAII design: ScreencopyManager owns the global; OutputEntry tracks each
// capturable output. When a client requests a frame, a FrameSession is created
// that owns the protocol resource and drives the capture lifecycle.
//
// Thread-safety: all calls must happen on the Wayland event loop thread.

module;

#include "luminaria/detail/wayland_fwd.h"

#include <cstdint>
#include <functional>
#include <typeinfo>
#include <memory>
#include <vector>

export module luminaria:screencopy;

import :core.expected;

export namespace luminaria {

class Display;
class VulkanRenderer;

/// Callback to capture pixels from an output region.
/// Fill `rgba` with tightly-packed RGBA8, row-major, top-to-bottom.
/// Return true on success. Called when a client's copy request arrives.
using ScreencopyCaptureFunc = std::function<bool(int x, int y, int w, int h,
                                                  std::vector<std::uint8_t>& rgba)>;

/// The wlr_screencopy_manager_v1 + ext_image_copy_capture_manager_v1 globals.
///
/// Usage:
///   1. Create the manager.
///   2. Register each output with add_output().
///   3. Clients can now capture screenshots.
///
/// Move-only; pointer-stable (pimpl) so the libwayland global is safe.
class ScreencopyManager {
public:
    [[nodiscard]] static Result<ScreencopyManager> create(Display& display);

    ~ScreencopyManager();
    ScreencopyManager(ScreencopyManager&&) noexcept;
    ScreencopyManager& operator=(ScreencopyManager&&) noexcept;
    ScreencopyManager(const ScreencopyManager&) = delete;
    ScreencopyManager& operator=(const ScreencopyManager&) = delete;

    /// Register an output for capture. `output` is the wl_resource the client
    /// passes to capture_output. `capture` is called when a client requests a copy.
    void add_output(wl_resource* output, int width, int height, ScreencopyCaptureFunc capture);

    /// Enable dmabuf capture targets. Clients may then hand a linux-dmabuf buffer
    /// instead of shm; LINEAR is written via mmap, tiled via the renderer. Without
    /// this, only shm targets are advertised.
    void set_renderer(VulkanRenderer* renderer);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit ScreencopyManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
