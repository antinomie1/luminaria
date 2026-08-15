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

/// One video mode a display can be driven at. `refresh_mhz` is millihertz —
/// the unit wl_output.mode uses, and the only one that can tell 59.94 from 60.
struct OutputMode {
    int width = 0;
    int height = 0;
    int refresh_mhz = 0;
    bool preferred = false; ///< the monitor's own idea of its native mode

    [[nodiscard]] bool operator==(const OutputMode&) const = default;
};

/// "This output is now driven at a different mode." Everything sized for the
/// old one is stale: render targets, scanout imports, the wl_output the clients
/// see, and this output's box in the layout.
struct OutputModeChange {
    Output& output;
    OutputMode mode;
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
    Signal<OutputModeChange> mode_changed;

    // --- asking for a frame ---
    //
    // An output that is not being committed to stops emitting `frame`. That is
    // deliberate and it is where the idle power goes: DRM's frames come from
    // page-flip completions, the nested backend's from the parent's frame
    // callbacks, and headless stops re-arming its timer — so a screen nothing
    // is changing on costs no vblank subscription, no kernel callback and no
    // process wake-up at all. `schedule_frame()` is how the loop starts again.
    //
    // A compositor built on `Frame` gets most of this for free: `Frame` asks on
    // its own behalf when a surface it drew commits, and `Frame::damage_all()`
    // asks too. Call it directly for anything else that changes pixels.

    /// Ask for one `frame` event, as soon as the display can pace one.
    /// Idempotent while a frame is already on its way, and safe to call from
    /// inside a `frame` handler — it asks for the next one.
    void schedule_frame() {
        if (frame_scheduled_) {
            return;
        }
        frame_scheduled_ = true;
        arm_frame();
    }

    /// True between `schedule_frame()` and the `frame` it asked for.
    [[nodiscard]] bool frame_scheduled() const noexcept { return frame_scheduled_; }

    // --- video modes ---

    /// Every mode this display reports, preferred first. Empty for backends
    /// with no fixed mode list at all (headless, nested), which take whatever
    /// size they were made with.
    [[nodiscard]] virtual std::vector<OutputMode> modes() const { return {}; }

    /// The mode currently driving the display. Backends with no mode list
    /// still answer with their size, so a caller never has to special-case it.
    [[nodiscard]] virtual OutputMode current_mode() const {
        return OutputMode{width_, height_, 0, true};
    }

    /// Drive this output at a different mode, matched against `modes()`. Pass
    /// `refresh_mhz = 0` to take the highest refresh rate at that size.
    ///
    /// On success `width()`/`height()` have changed and `mode_changed` has
    /// fired — every scanout id handed out before is invalid, and anything the
    /// compositor sized for this output has to be rebuilt. Switching to the
    /// mode already in use is a successful no-op and fires nothing.
    virtual Status set_mode(int width, int height, int refresh_mhz = 0) {
        (void)refresh_mhz;
        return width == width_ && height == height_
                   ? ok()
                   : fail("this output cannot change mode");
    }

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

    /// Drop an import. A compositor's own render targets live as long as the
    /// output does and never need this; buffers that came from a *client* do —
    /// a toolkit drops its whole swapchain on every resize, and without this
    /// the imports pile up for the life of the output. Releasing an id that
    /// was never imported, or is currently on screen, is a no-op: the backend
    /// keeps the scanned-out buffer alive until something replaces it.
    virtual void release_scanout(std::uint32_t id) { (void)id; }

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

    /// Make the backend deliver one `frame` event. Called at most once per
    /// `schedule_frame()`, and only when no frame was already pending. Backends
    /// whose frames are already self-clocking (a page flip is in flight, the
    /// parent owes us a callback) may do nothing here — the flag is cleared by
    /// the emit either way.
    virtual void arm_frame() {}

    /// A `frame` event is being delivered now: the request is spent, and a
    /// handler asking for another one must be able to get it.
    void frame_delivered() noexcept { frame_scheduled_ = false; }

    /// Report a frame with a software timestamp, then ask for the next one.
    /// For backends with no vblank of their own (headless, nested): the clock is
    /// still CLOCK_MONOTONIC, so clients get usable timing, just not hw_clock.
    void emit_software_frame(std::uint32_t refresh_ns = 0) {
        frame_delivered();
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

    bool frame_scheduled_ = false;
    int width_;
    int height_;
    int scale_ = 1;
    Transform transform_ = Transform::normal;
    Color last_committed_{};
};

} // namespace luminaria
