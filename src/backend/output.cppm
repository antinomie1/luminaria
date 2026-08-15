// luminaria/output.cppm — a display output the compositor renders to.
//
// Backend-specific behaviour (headless, nested, DRM) lives in subclasses; the
// compositor only sees this interface. `frame` fires when it's time to draw the
// next frame; the compositor responds by committing new content.

module;

#include <cstdint>
#include <ctime>
#include <span>
#include <vector>
#include <unistd.h> // close, for the fences that cross this interface

export module luminaria:output;

import :box;
import :color;
import :dmabuf;
import :expected;
import :pixel;
import :signal;
import :transform;

export namespace luminaria {

class Output;

/// "Time to draw a new frame on `output`."
struct FrameEvent {
    Output& output;
};

/// "This output is going away" — a monitor was unplugged, or the backend shut
/// down. Anything holding an `Output*` (layouts, per-output render state, the
/// wl_output global) must drop it here. The backend part of the object is
/// already gone by the time this fires: drop the pointer, don't call into it.
struct OutputDestroy {
    Output& output;
};

/// "The frame committed earlier is now on screen." Fires just before `frame`,
/// carrying the timestamp the display hardware reported. This is what
/// wl_surface.frame callbacks and wp_presentation feedback must be paced by —
/// acknowledging at commit time instead makes clients render frames nobody sees.
struct PresentEvent {
    Output& output;
    std::uint64_t tv_sec = 0;      // CLOCK_MONOTONIC
    std::uint32_t tv_nsec = 0;
    std::uint32_t refresh_ns = 0;  // nominal frame duration, 0 if unknown
    std::uint64_t seq = 0;         // vblank counter, 0 if unknown
    bool vsync = true;             // false when the flip tore (async page-flip)
    bool hw_clock = false;         // the timestamp came from the display hardware

    /// Milliseconds for wl_callback.done, which is all wl_surface.frame carries.
    [[nodiscard]] std::uint32_t time_ms() const noexcept {
        return static_cast<std::uint32_t>(tv_sec * 1000 + tv_nsec / 1000000);
    }
};

class Output {
public:
    virtual ~Output() {
        OutputDestroy event{*this};
        destroy.emit(event);
    }
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    /// Size of the framebuffer in real pixels — the mode, untouched by scale or
    /// rotation. Renderers allocate this; almost everything else wants the
    /// logical size below.
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

    // --- scale and rotation ---
    //
    // `scale` is the integer output scale (2 = HiDPI); `transform` is how
    // logical content is rotated/reflected to land on the framebuffer. Together
    // they define the logical size, which is what windows are laid out in.

    [[nodiscard]] int scale() const noexcept { return scale_; }
    void set_scale(int scale) noexcept { scale_ = scale < 1 ? 1 : scale; }
    [[nodiscard]] Transform transform() const noexcept { return transform_; }
    void set_transform(Transform transform) noexcept { transform_ = transform; }

    [[nodiscard]] int logical_width() const noexcept {
        return (transform_swaps_axes(transform_) ? height_ : width_) / scale_;
    }
    [[nodiscard]] int logical_height() const noexcept {
        return (transform_swaps_axes(transform_) ? width_ : height_) / scale_;
    }

    /// A box in this output's logical coordinates, in framebuffer pixels.
    [[nodiscard]] Box to_device(const Box& logical) const noexcept {
        return transform_box(transform_, scale_, logical, width_, height_);
    }

    Signal<FrameEvent> frame;
    Signal<PresentEvent> present;
    Signal<OutputDestroy> destroy;

    /// Ask for the next commits to be shown without waiting for vblank
    /// (wp_tearing_control_v1). Backends that can't tear ignore it.
    virtual void set_tearing(bool async) { (void)async; }

    /// Present a solid color (convenience / bring-up path).
    virtual Status commit(Color color) = 0;

    /// Present a rendered RGBA frame (row-major, width*height pixels). This is the
    /// real compositing path: render the scene, then hand the pixels to scanout.
    /// Default: unsupported (backends that can scan out pixels override it).
    virtual Status commit_frame(std::span<const Pixel> rgba, int width, int height) {
        (void)rgba;
        (void)width;
        (void)height;
        return fail("this output cannot present a pixel frame");
    }

    // --- GPU scanout: present a renderer-produced dmabuf with no copy at all ---

    /// DRM modifiers this output's scanout hardware accepts for `drm_format`.
    /// Intersect with VulkanRenderer::scanout_modifiers() before allocating a
    /// ScanoutTarget; an empty intersection means this output has no zero-copy
    /// path and the frame has to go through `commit_frame` instead.
    ///
    /// Default: empty, which is the honest answer for an output that does not
    /// override `import_scanout` either. Claiming LINEAR here would only send
    /// the caller off to allocate a target it is then refused.
    [[nodiscard]] virtual std::vector<std::uint64_t> scanout_modifiers(std::uint32_t drm_format) {
        (void)drm_format;
        return {};
    }

    /// Register a dmabuf as a scanout framebuffer, returning an opaque id valid
    /// until this output dies. Import each target once, then flip between them.
    /// Default: unsupported.
    [[nodiscard]] virtual Result<std::uint32_t> import_scanout(const DmabufPlane& plane) {
        (void)plane;
        return fail("this output cannot scan out a dmabuf");
    }

    /// Page-flip to a buffer returned by import_scanout(). `frame` fires again
    /// once the flip completes.
    ///
    /// `in_fence_fd` (owned by this call, -1 for none) is a sync_file the display
    /// hardware waits on before it scans the buffer out — pass the out-fence from
    /// `VulkanRenderer::render_to` and the frame goes from client to screen with
    /// nothing blocking on the CPU at any point.
    virtual Status commit_scanout(std::uint32_t id, int in_fence_fd = -1) {
        (void)id;
        if (in_fence_fd >= 0) {
            ::close(in_fence_fd);
        }
        return fail("this output cannot scan out a dmabuf");
    }

    /// A sync_file that signals when the last committed frame is actually on
    /// screen — which is when the buffer it replaced becomes safe to draw into
    /// again. The caller owns it; -1 if the backend has no such fence. Hand it
    /// to `ScanoutTarget::set_acquire_fence()` and the next render waits on the
    /// GPU instead of on the page-flip event.
    [[nodiscard]] virtual int take_present_fence() noexcept { return -1; }

    // --- hardware cursor plane ---
    //
    // Moving the pointer is the most frequent thing a desktop does, and it does
    // not need the screen recomposited. A KMS cursor plane moves on its own, so
    // `move_cursor` is one small atomic commit rather than a full frame.

    /// True if this output has a cursor plane at all. When false the compositor
    /// has to composite the pointer itself, and the calls below do nothing.
    [[nodiscard]] virtual bool has_cursor_plane() const noexcept { return false; }

    /// Upload a cursor image. `rgba` is tightly packed, premultiplied RGBA8 of
    /// `width`x`height`; the hotspot is in the same pixels. Images larger than
    /// the hardware allows are rejected — fall back to compositing then.
    virtual Status set_cursor(std::span<const std::uint8_t> rgba, int width, int height,
                              int hotspot_x, int hotspot_y) {
        (void)rgba;
        (void)width;
        (void)height;
        (void)hotspot_x;
        (void)hotspot_y;
        return fail("this output has no cursor plane");
    }

    /// Position the cursor, in this output's LOGICAL coordinates (the hotspot
    /// lands on x,y). No repaint of anything else.
    virtual Status move_cursor(int x, int y) {
        (void)x;
        (void)y;
        return fail("this output has no cursor plane");
    }

    /// Take the cursor off the screen (the pointer left this output).
    virtual Status hide_cursor() { return fail("this output has no cursor plane"); }

    [[nodiscard]] Color last_committed() const noexcept { return last_committed_; }

protected:
    Output(int width, int height) noexcept : width_(width), height_(height) {}

    /// Report a frame with a software timestamp, then ask for the next one.
    /// For backends with no vblank of their own (headless, nested): the clock is
    /// still CLOCK_MONOTONIC, so clients get usable timing, just not hw_clock.
    void emit_software_frame(std::uint32_t refresh_ns = 0) {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        PresentEvent presented{*this,
                               static_cast<std::uint64_t>(ts.tv_sec),
                               static_cast<std::uint32_t>(ts.tv_nsec),
                               refresh_ns,
                               0,
                               true,
                               false};
        present.emit(presented);
        FrameEvent next{*this};
        frame.emit(next);
    }

    int width_;
    int height_;
    int scale_ = 1;
    Transform transform_ = Transform::normal;
    Color last_committed_{};
};

} // namespace luminaria
