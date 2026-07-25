// luminaria/drm_syncobj.cppm — explicit GPU synchronisation (linux-drm-syncobj-v1).
//
// Modern GPU clients (Mesa, Vulkan/GL toolkits) no longer want implicit fences
// on their dmabufs. Instead each commit names two points on a DRM timeline
// syncobj the client imported:
//   * acquire — the compositor must not read the buffer until it is signalled,
//   * release — the compositor signals it once the buffer is free for reuse.
//
// luminaria composites through a CPU readback, so the acquire wait is a plain
// blocking `drmSyncobjTimelineWait` at commit time (bounded, so a broken client
// can't wedge the loop) and the release point is signalled when the buffer is
// superseded — exactly where wl_buffer.release would have been sent.

module;

#include <memory>

export module luminaria:drm_syncobj;

import :core.expected;

export namespace luminaria {

class Display;

/// The wp_linux_drm_syncobj_manager_v1 global (version 1). Move-only;
/// pointer-stable state so the libwayland global can hold a pointer to it.
class DrmSyncobjManager {
public:
    /// Open a DRM render node (for importing client syncobjs) and create the
    /// global. Fails if no render node is usable or it lacks timeline syncobj
    /// support (`DRM_CAP_SYNCOBJ_TIMELINE`).
    [[nodiscard]] static Result<DrmSyncobjManager> create(Display& display);

    ~DrmSyncobjManager();
    DrmSyncobjManager(DrmSyncobjManager&&) noexcept;
    DrmSyncobjManager& operator=(DrmSyncobjManager&&) noexcept;
    DrmSyncobjManager(const DrmSyncobjManager&) = delete;
    DrmSyncobjManager& operator=(const DrmSyncobjManager&) = delete;

    /// How long a commit may block waiting for an acquire point, in
    /// milliseconds. Defaults to 50; a client that misses it gets its frame
    /// composited anyway rather than stalling the compositor.
    void set_acquire_timeout_ms(unsigned ms) noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit DrmSyncobjManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
