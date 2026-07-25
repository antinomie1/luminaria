// luminaria/render/vulkan.hpp — Vulkan renderer (Vulkan-Hpp RAII under the hood).
//
// Public header leaks no Vulkan headers: all vk::raii state hides behind Impl.
// Vulkan-Hpp uses exceptions internally; they are caught at every method
// boundary and turned into Result, so nothing throws across a C callback.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "luminaria/core/expected.hpp"
#include "luminaria/util/color.hpp"
#include "luminaria/util/dmabuf.hpp"
#include "luminaria/util/pixel.hpp"
#include "luminaria/util/rect_fill.hpp"
#include "luminaria/util/region.hpp"
#include "luminaria/util/transform.hpp"

namespace luminaria {

/// A surface's pixels placed at (x,y). `rgba` is w*h*4 tightly-packed RGBA8.
struct TextureFill {
    int x, y, w, h;
    const std::uint8_t* rgba;
};

/// A GPU-resident image the renderer composites from: either a client dmabuf
/// imported with no copy at all, or CPU pixels uploaded once. Move-only, and it
/// must not outlive the VulkanRenderer that made it.
class GpuTexture {
public:
    ~GpuTexture();
    GpuTexture(GpuTexture&&) noexcept;
    GpuTexture& operator=(GpuTexture&&) noexcept;
    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;

    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;

    struct Impl;

private:
    friend class VulkanRenderer;
    std::unique_ptr<Impl> impl_;
    explicit GpuTexture(std::unique_ptr<Impl> impl) noexcept;
};

/// A GPU render target that is also a dmabuf, so KMS can scan it out directly:
/// the compositing result never touches system memory. Move-only, and it must
/// not outlive the VulkanRenderer that made it.
class ScanoutTarget {
public:
    ~ScanoutTarget();
    ScanoutTarget(ScanoutTarget&&) noexcept;
    ScanoutTarget& operator=(ScanoutTarget&&) noexcept;
    ScanoutTarget(const ScanoutTarget&) = delete;
    ScanoutTarget& operator=(const ScanoutTarget&) = delete;

    /// The exported plane. `fd` is owned by this target — borrow, don't close.
    [[nodiscard]] const DmabufPlane& plane() const noexcept;

    /// A sync_file the GPU must wait for before drawing into this target again:
    /// the KMS out-fence of the flip that is still scanning it out. Takes
    /// ownership (-1 is a no-op); the next render_to waits on it and closes it.
    /// Without this the caller has to keep the display and the GPU in step by
    /// blocking, which is the stall explicit sync exists to remove.
    void set_acquire_fence(int fd) noexcept;
    [[nodiscard]] int width() const noexcept { return plane().width; }
    [[nodiscard]] int height() const noexcept { return plane().height; }

    struct Impl;

private:
    friend class VulkanRenderer;
    std::unique_ptr<Impl> impl_;
    explicit ScanoutTarget(std::unique_ptr<Impl> impl) noexcept;
};

/// A GpuTexture stretched into the destination rect (x,y,w,h), in the output's
/// LOGICAL coordinates — the renderer applies scale and rotation.
struct GpuTextureFill {
    const GpuTexture* texture;
    int x, y, w, h;

    /// How the source buffer is oriented relative to the surface — pass
    /// `Surface::buffer_transform()`. Folded into the output's own rotation so
    /// a rotated client on a rotated screen is still one sample.
    Transform transform = Transform::normal;

    /// A sub-rect of the destination the caller guarantees is fully opaque, in
    /// logical coordinates. Whatever is behind it is not drawn at all.
    /// Empty (the default) means "assume translucent".
    // ponytail: one box, not a region — clients that declare an opaque region
    // overwhelmingly declare the whole surface. Widen it if one ever doesn't.
    Box opaque{};

    /// Source crop, normalized against the BUFFER (before `transform`).
    /// Defaults to the whole texture; wp_viewporter narrows it.
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
};

/// How an output's logical coordinates map onto its framebuffer: the integer
/// scale (2 for HiDPI) and the rotation/reflection of the panel.
struct OutputMapping {
    Transform transform = Transform::normal;
    int scale = 1;
};

/// GPU-side synchronisation for one render: fences in, a fence out, no CPU
/// stall in between. This is what makes explicit sync worth having — a client's
/// acquire point becomes a semaphore the GPU waits on, and the finished render
/// becomes a fence KMS waits on (`IN_FENCE_FD` on the atomic commit).
struct RenderSync {
    /// sync_file fds the render must wait for before sampling — a client's
    /// linux-drm-syncobj acquire point. Borrowed; the caller keeps ownership.
    std::span<const int> wait_fds{};

    /// If non-null, receives a sync_file fd that signals when the render is
    /// done, and render_to returns without waiting. The caller OWNS it: hand it
    /// to `Output::commit_scanout()`, or close it. Left null, render_to blocks
    /// on a fence as before, which is what the CPU read-back paths need.
    int* out_fence_fd = nullptr;
};

class VulkanRenderer {
public:
    /// Bring up an instance + device. Fails if no Vulkan device is available.
    [[nodiscard]] static Result<VulkanRenderer> create();

    ~VulkanRenderer();
    VulkanRenderer(VulkanRenderer&&) noexcept;
    VulkanRenderer& operator=(VulkanRenderer&&) noexcept;
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    /// Render a solid-color frame offscreen and read back pixel (0,0). Proves the
    /// GPU path end to end; real surface compositing arrives in Phase 2.
    [[nodiscard]] Result<Pixel> render_clear_readback(int width, int height, Color color);

    /// Composite: clear to `background`, then paint each RectFill (back-to-front)
    /// into a width×height frame. Returns the frame as row-major RGBA pixels.
    [[nodiscard]] Result<std::vector<Pixel>> render_rects(int width, int height, Color background,
                                                          std::span<const RectFill> rects);

    /// Composite rects, then blit surface textures (opaque, 1:1) on top at their
    /// positions. `rgba` is tightly-packed w×h RGBA8. Returns row-major RGBA.
    ///
    /// Copy-based placement: no scaling, no rotation, no alpha blending. This is
    /// the fallback for backends that cannot scan out a dmabuf (headless,
    /// nested) and for screencopy read-back. Anything that can use `render_to`
    /// should — that one is the real pipeline.
    [[nodiscard]] Result<std::vector<Pixel>> composite(int width, int height, Color background,
                                                       std::span<const RectFill> rects,
                                                       std::span<const TextureFill> textures);

    // --- GPU compositing path (no CPU read-back anywhere) ---
    //
    // import_texture(client dmabuf) / upload_texture(shm pixels)
    //   -> render_to(scanout target)
    //   -> Output::import_scanout + commit_scanout (KMS atomic page-flip).

    /// Import a client dmabuf as a sampled texture. Nothing is copied: the GPU
    /// reads the client's pages directly. `plane.fd` is borrowed (dup'd inside).
    [[nodiscard]] Result<GpuTexture> import_texture(const DmabufPlane& plane);

    /// Upload tightly-packed RGBA8 into a device-local texture (the wl_shm path).
    [[nodiscard]] Result<GpuTexture> upload_texture(int width, int height,
                                                     std::span<const std::uint8_t> rgba);

    /// DRM modifiers this GPU can render into *and* export as a dmabuf for
    /// `drm_format`. Intersect with what the display hardware can scan out.
    [[nodiscard]] std::vector<std::uint64_t> scanout_modifiers(std::uint32_t drm_format);

    /// Allocate a render target exported as a dmabuf. The driver picks one of
    /// `modifiers` (pass the intersection of scanout_modifiers() and what the
    /// output accepts); an empty list means LINEAR only.
    [[nodiscard]] Result<ScanoutTarget> create_scanout(int width, int height,
                                                        std::uint32_t drm_format,
                                                        std::span<const std::uint64_t> modifiers);

    /// Composite straight into a scanout target: clear to `background`, paint the
    /// rects, then draw each texture into its destination rect (scaling and
    /// rotating as needed), back to front.
    ///
    /// Rects, textures and damage are all in the output's logical coordinates;
    /// `mapping` turns those into framebuffer pixels, so a rotated or HiDPI
    /// output needs no special-casing from the caller.
    ///
    /// `damage` limits the work to the regions that actually changed, in logical
    /// coordinates; empty means "redraw everything". It is turned into a set of
    /// disjoint boxes and each is scissored separately — two small dirty corners
    /// cost two small scissors, not the rectangle spanning them. The rest of the
    /// target keeps the pixels it already had, so with N buffers in rotation the
    /// caller must pass the union of the last N frames' damage — this target's
    /// content is N frames old, not one.
    ///
    /// Anything hidden behind a `GpuTextureFill::opaque` rect is skipped.
    ///
    /// `sync` decides how this call joins up with the GPU: by default it blocks
    /// on a fence and the caller may page-flip on return. Pass wait fds and ask
    /// for an out-fence instead and nothing waits on the CPU at all — see
    /// RenderSync.
    [[nodiscard]] Status render_to(ScanoutTarget& target, Color background,
                                   std::span<const RectFill> rects,
                                   std::span<const GpuTextureFill> textures,
                                   std::span<const Box> damage = {},
                                   const OutputMapping& mapping = {},
                                   const RenderSync& sync = {});

    // --- linux-dmabuf import/export (any DRM modifier the GPU supports) ---

    /// True if the device exposes the external-memory-dmabuf + DRM-modifier
    /// extensions, i.e. import/export below actually work.
    [[nodiscard]] bool dmabuf_supported() const noexcept;

    /// DRM modifiers usable for import+export of `drm_format` (single-plane,
    /// transfer-capable). Always includes DRM_FORMAT_MOD_LINEAR when supported.
    /// Empty if dmabuf is unsupported or the format has no usable modifier.
    [[nodiscard]] std::vector<std::uint64_t> dmabuf_modifiers(std::uint32_t drm_format);

    /// Read a scanout target back as tightly-packed RGBA8, for the backends that
    /// present CPU pixels (headless, nested wl_shm) and for screencopy.
    ///
    /// Copies from the target's own VkImage — the renderer already owns it, so
    /// nothing is imported — through a staging buffer the target keeps and
    /// reuses, in host-CACHED memory where the driver has any. Re-importing the
    /// dmabuf and reading uncached memory every frame instead costs ~90ms on a
    /// 800x600 frame, which is the difference between a smooth pointer and a
    /// visibly lagging one.
    ///
    /// `out` is resized to width*height*4.
    [[nodiscard]] Status read_scanout(ScanoutTarget& target, std::vector<std::uint8_t>& out);

    /// Import a single-plane dmabuf (ARGB8888/XRGB8888, any modifier) and read it
    /// back as tightly-packed RGBA8. `fd` is borrowed (dup'd internally). For a
    /// target this renderer made, prefer `read_scanout()` — it is far cheaper.
    [[nodiscard]] Result<std::vector<std::uint8_t>> import_dmabuf(int fd, int width, int height,
                                                                  std::uint32_t drm_format,
                                                                  std::uint32_t offset,
                                                                  std::uint32_t stride,
                                                                  std::uint64_t modifier);

    /// Write tightly-packed RGBA8 into a single-plane dmabuf target (screencopy).
    /// `fd` is borrowed (dup'd internally).
    [[nodiscard]] Status export_dmabuf(int fd, int width, int height, std::uint32_t drm_format,
                                       std::uint32_t offset, std::uint32_t stride,
                                       std::uint64_t modifier,
                                       const std::vector<std::uint8_t>& rgba);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit VulkanRenderer(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
