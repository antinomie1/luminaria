// luminaria/shell/frame.cppm — the per-output frame ledger.
//
// Every compositor written against this library ends up writing the same three
// hundred lines: build a z-ordered list of what to draw, work out how much of
// the target is actually stale, decide whether the whole composite can be
// skipped in favour of a client's own buffer, thread the fences between the GPU
// and the display, and flip. Getting any of it subtly wrong shows up as
// tearing, as a frame of latency, or as a client that never redraws — so it is
// not left to each compositor to rediscover. `Frame` is that ledger.
//
// It is immediate-mode on purpose (see docs/adr/0001): the compositor owns the
// windows and refills the placement list every time it needs one. There is no
// tree here to keep in sync with the compositor's own model, and nothing about
// a window survives from one frame to the next. What DOES survive is the memory
// — the vectors are cleared, never freed — and the two things that are
// genuinely about frames rather than windows: the damage each buffer in the
// rotation still owes, and the buffers held for the display.
//
//     Frame frame{output, renderer};
//     (void)frame.reset(DRM_FORMAT_XRGB8888);      // and again on mode_changed
//     ...
//     output.frame -> {
//         frame.begin(layout.box_of(output));
//         for (Window& w : windows) frame.place(w.surface(), w.x, w.y);
//         (void)frame.submit(background);
//     }
//     output.present -> frame.presented();
//
// The list is also what hit-testing must run against, so that a click can never
// land somewhere the pixels are not. Rebuild it (`begin` + `place`) at input
// time as well — that is the cheap half. Placements retain generational ids, so
// even a list that accidentally survives a dispatch cannot dereference a dead
// surface or a new surface that reused its registry slot.

module;


#include <unistd.h> // dup, close — the fences handed between GPU and display

export module luminaria.gpu:frame;

import std;

import luminaria;
import :direct_scanout;
import :vulkan;

export namespace luminaria {

/// One "draw this here, this big" record: a surface (with its buffer, crop and
/// transform) or a bare texture, placed in LAYOUT coordinates.
///
/// A frame's worth of these is the whole shell-layer state. They are refilled
/// from scratch by `Frame::begin()`/`place()` and must not be stored across a
/// dispatch; the SurfaceId still makes accidental retention memory-safe.
struct Placement {
    /// The client surface, or an invalid id for a compositor-owned texture (a
    /// themed cursor drawn into the frame, say).
    SurfaceId surface;
    /// Resolved at `submit()` time; null when the surface has no usable buffer
    /// yet, in which case the placement is still hit-testable but not drawn.
    const GpuTexture* texture = nullptr;

    float x = 0, y = 0;          ///< top-left, layout coordinates
    float width = 0, height = 0; ///< surface coordinates, not buffer pixels

    // The inverse map used only for a transformed client surface's input.
    // Ordinary placements retain the defaults: `(layout - x) * 1 + 0`.
    float input_x = 0.0f, input_y = 0.0f;
    float input_scale_x = 1.0f, input_scale_y = 1.0f;

    Transform transform = Transform::normal;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;

    /// Whole-placement opacity. Client surfaces always use 1; compositor-owned
    /// textures may use less, normally after a whole window was first composed
    /// into an OffscreenTarget. A translucent placement must not claim opacity.
    float alpha = 1.0f;

    /// False only for a placement absorbed into a composed group. It stays in
    /// the list for hit-testing, but its pixels are represented by the group's
    /// one texture placement instead.
    bool draw = true;

    /// Solid rectangle: when set, this placement paints `color` instead of a
    /// surface or texture (which must then be absent). Compositor-owned —
    /// borders, masks, cursor backdrops. Not hit-testable, and it participates
    /// in the placement diff like any other primitive, so moving it or changing
    /// its colour costs exactly the two rectangles.
    bool solid = false;
    Color color{};

    /// Rounded corners, radius in layout pixels. A solid placement with
    /// `feather` above 0 is a drop shadow instead: it fades out over that many
    /// pixels *outside* its box, following the same corner radius.
    float corner_radius = 0.0f;
    float feather = 0.0f;

    /// The opaque region, in layout coordinates, as `[first, first+count)` in
    /// the frame's arena — ask `Frame::opaque_of()`. It lives here as a pair of
    /// indices rather than a `Region` because a `Region` per surface per frame
    /// is a heap allocation per surface per frame, which is precisely the cost
    /// this layer exists to not pay.
    std::uint32_t opaque_first = 0;
    std::uint32_t opaque_count = 0;

    /// Which picture a compositor-owned texture is currently holding.
    ///
    /// The damage diff identifies a texture by its address, which is right for
    /// a surface (a new buffer is a new texture) and wrong for a caller that
    /// re-uploads new pixels into a texture it keeps — a bar redrawn for a
    /// workspace switch is a different picture at the same address, and without
    /// this the frame diffs as unchanged and the screen keeps the old one.
    std::uint64_t content = 0;
};

/// A visual placement's transform. It is a small immutable value object so an
/// animation can build crop, scale, move and opacity in one place, then hand
/// the finished result to `Frame` for a surface tree or a compositor texture.
///
/// `crop()`'s rectangle is normalized to the transform's CURRENT visible
/// source rectangle: `(0,0,1,1)` keeps it all, `(0.25,0,0.5,1)` keeps its
/// middle half. This makes repeated crops compose instead of replacing one
/// another. `rescale()` is around the top-left; call `relocate()` afterwards
/// when the animation needs a different anchor.
class PlacementTransform {
public:
    [[nodiscard]] static PlacementTransform at(float x, float y, float width,
                                               float height) noexcept {
        PlacementTransform result{};
        result.x_ = x;
        result.y_ = y;
        result.width_ = width;
        result.height_ = height;
        return result;
    }

    /// Keep a normalized sub-rectangle of the currently visible texture.
    [[nodiscard]] PlacementTransform crop(float x, float y, float width,
                                          float height) const noexcept;

    /// Move the destination rectangle without changing its size or crop.
    [[nodiscard]] PlacementTransform relocate(float x, float y) const noexcept;

    /// Scale the destination rectangle around its top-left corner.
    [[nodiscard]] PlacementTransform rescale(float x, float y) const noexcept;

    /// Set whole-quad opacity, clamped to `[0, 1]`.
    [[nodiscard]] PlacementTransform opacity(float alpha) const noexcept;

    /// Cut rounded corners of `radius` layout pixels out of the destination.
    ///
    /// Honoured on a texture placement, ignored on a surface tree: masking each
    /// surface of a tree separately would round a subsurface's own corners in
    /// the middle of the window. A client surface therefore gets its corners by
    /// going through `compose_group()` first and rounding the one texture that
    /// comes out — the same route, and the same cost, as fading a window.
    [[nodiscard]] PlacementTransform rounded(float radius) const noexcept;

private:
    friend class Frame;

    float x_ = 0.0f, y_ = 0.0f;
    float width_ = 0.0f, height_ = 0.0f;
    float u0_ = 0.0f, v0_ = 0.0f, u1_ = 1.0f, v1_ = 1.0f;
    float alpha_ = 1.0f;
    float radius_ = 0.0f;
};

/// A marker returned by `Frame::begin_group()`. It names the suffix of the
/// current placement list added after the marker; it is invalid after `begin()`.
class PlacementGroup {
public:
    PlacementGroup() = default;

private:
    friend class Frame;
    std::size_t first_ = 0;
    std::uint64_t generation_ = 0;
    PlacementGroup(std::size_t first, std::uint64_t generation) noexcept
        : first_(first), generation_(generation) {}
};

/// What `Frame::submit()` did with the frame.
enum class Presented {
    /// Composited into a render target and flipped.
    composited,
    /// A client's own buffer went straight to the display; nothing was drawn.
    scanout,
    /// Nothing had changed, so the frame already on screen is still the right
    /// one: nothing was drawn and nothing was committed, and the output will
    /// now stop emitting `frame` until something asks it for one. This is where
    /// an idle desktop's power goes.
    ///
    /// It also means no `present` event follows. Whatever the compositor does
    /// there — `Surface::send_frame_done()` above all — it must do on this
    /// answer too, or a client that committed without damaging anything waits
    /// for a callback that is never sent.
    unchanged,
    /// No render target at all: a solid background was committed instead.
    fallback,
};

/// The per-output frame ledger. Move-only, and it must not outlive either the
/// output or the renderer it was built from.
class Frame {
public:
    Frame(Output& output, VulkanRenderer& renderer);

    ~Frame();
    Frame(Frame&&) noexcept;
    Frame& operator=(Frame&&) noexcept;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    /// Allocate everything sized for the output's CURRENT mode: the pair of
    /// scanout buffers, and the direct-scanout cache. Call it once after
    /// construction and again from `Output::mode_changed` — after a mode switch
    /// every one of those describes a resolution that is no longer on the wire.
    ///
    /// `drm_format` is what the display scans out; XRGB8888 is the one format
    /// every KMS primary plane takes. Failing here is not fatal: the frame falls
    /// back to a CPU read-back, and then to a solid colour, and says which in
    /// `Presented`.
    Status reset(std::uint32_t drm_format);

    /// Start a new placement list. `view` is this output's rectangle in layout
    /// coordinates (`OutputLayout::box_of`), and everything placed is clipped
    /// against it. Keeps the capacity of every buffer it clears.
    void begin(const Box& view);

    /// Place `surface` and its whole subsurface tree, back-to-front, with the
    /// tree's root at (x,y) in layout coordinates. Surfaces that miss this
    /// output's view entirely are dropped here rather than carried to the GPU.
    void place(Surface& surface, int x, int y);

    /// Place a surface tree through a composable crop / scale / move transform.
    /// An explicit crop is in the root surface's normalized logical coordinates;
    /// an uncropped transform retains children outside the root. Hit-testing
    /// applies the inverse scale before consulting each surface's input region.
    /// For opacity below one use `compose_group()` instead: applying it per
    /// child exposes seams wherever subsurfaces or popups overlap.
    void place(Surface& surface, const PlacementTransform& transform);

    /// Place a texture the compositor owns — a themed cursor on a backend with
    /// no cursor plane. It is not hit-testable, and it reports no damage of its
    /// own; moving it or swapping the texture is nevertheless picked up, because
    /// `submit()` diffs this frame's list against the last one it drew.
    void place(const GpuTexture& texture, int x, int y, int width, int height);

    /// As above, with whole-quad opacity. Use this for a compositor-owned
    /// texture such as an OffscreenTarget's finished window.
    void place(const GpuTexture& texture, int x, int y, int width, int height, float alpha);

    /// Floating-point variant for an animated compositor-owned texture. The
    /// GPU consumes the exact geometry; damage and culling round its coverage
    /// outward to pixels, so a subpixel move cannot leave a stale edge.
    void place(const GpuTexture& texture, float x, float y, float width, float height,
               float alpha = 1.0f);

    /// Place a compositor-owned texture through a composable crop / scale /
    /// move / opacity transform. Invalid or empty transforms draw nothing.
    ///
    /// `content` names the picture the texture is holding, for a caller that
    /// re-uploads into the same texture across frames: bump it and the damage
    /// diff repaints, leave it and an unchanged texture still costs nothing.
    void place(const GpuTexture& texture, const PlacementTransform& transform,
               std::uint64_t content = 0);

    /// As above, but run `shader` as the fragment stage. The shader's mandatory
    /// ShaderDamage declaration controls full repaint and continuous frames.
    void place(const GpuTexture& texture, const PlacementTransform& transform,
               const FragmentShader& shader);

    /// Place a drop shadow for `box`: a solid `color` fading to nothing over
    /// `feather` layout pixels outside it, following `radius` at the corners.
    ///
    /// The shadow is drawn as the rectangle it surrounds, grown by `feather` —
    /// so pass the box that casts it, and place it immediately *before* that
    /// box's own placement. Nothing is painted inside the caster, which is why
    /// a translucent window is not darkened by its own shadow.
    ///
    /// Analytic, not blurred: no extra pass and no extra target, so a shadow
    /// costs one more quad and nothing else.
    void place_shadow(const Box& box, Color color, float radius, float feather);

    /// Place a solid rectangle the compositor owns — a background panel, a
    /// border, a mask, a cursor backdrop. It is not hit-testable and reports no
    /// damage of its own; moving it or changing its colour is nevertheless
    /// picked up, because `submit()` diffs this frame's list against the last
    /// one it drew. It sits in the z-order like anything else: place it where
    /// it belongs among the surfaces.
    void place_rect(int x, int y, int width, int height, Color color);

    /// Place one blur placement per region a surface tree declared through
    /// ext-background-effect-v1, translated by the root's origin and clipped to
    /// each child's own surface, `clip`, the output view and `background`. Call
    /// immediately before the ordinary `place(surface, x, y)`: these texture
    /// placements are part of the same diff/damage ledger but intentionally
    /// have no input target.
    ///
    /// `spread` is `blur_spread()` for the parameters the texture was made
    /// with; each placed region is recorded with it, so `submit()` expands the
    /// region's damage correctly when something near it changes. Answers
    /// whether at least one region was placed — an empty client request must
    /// not buy a backdrop capture.
    [[nodiscard]] bool place_blur_regions(const GpuTexture& texture, const Box& background,
                                          Surface& surface, int x, int y, const Box& clip,
                                          int spread);

    /// Composite everything placed since `group` into `target`, blur it into
    /// `chain`, and leave the placements alone.
    ///
    /// This is the expensive kind of blur, and the cost is visible in that
    /// sentence: unlike `compose_group`, which replaces what it composited,
    /// the pixels below a blurred window are drawn twice — once to be blurred,
    /// once for real. What buys is that the blur source is the actual scene, so
    /// a window blurs the window behind it and not just the wallpaper, which is
    /// the whole difference from `place_xray_blur`.
    ///
    /// `bounds` is what `target` covers, in layout coordinates, and `chain`
    /// must have been created at `target`'s size. Follow it with `place_blur`
    /// for each region that shows the result.
    [[nodiscard]] Status capture_blur(PlacementGroup group, OffscreenTarget& target,
                                      const Box& bounds, BlurChain& chain,
                                      const BlurParams& params);

    /// Place a blur backdrop: `texture` — normally `BlurChain::texture()` —
    /// addressed as covering `background`, shown only inside `box`, with
    /// `radius` rounded corners so it does not spill outside the window it is
    /// the backdrop for.
    ///
    /// `spread` is `blur_spread()` for the parameters it was made with. It is
    /// not decoration: it tells `submit()` how far away a change still makes
    /// this box stale, and getting it wrong leaves a smear on the screen.
    void place_blur(const GpuTexture& texture, const Box& background, const Box& box,
                    float radius, int spread);

    /// Queue a backdrop capture and add the chain's texture as a blur placement
    /// covering `box`.
    ///
    /// This is `capture_blur` + `place_blur` with the capture moved into
    /// `submit()`: the placement is in the list from now, so the unchanged diff
    /// sees it like any other, but the offscreen render and the blur chain only
    /// run if the frame actually repaints — an unchanged frame must not pay for
    /// a backdrop it never shows. `target` and `chain` are borrowed and must
    /// outlive this frame's `submit()`; the queued job owns only values, and
    /// `begin()` clears every job.
    ///
    /// `box` is the blur's visible rectangle, `radius` its corner rounding and
    /// `spread` its damage reach — the same arguments `place_blur` takes.
    /// Answers false when the capture could not be queued, in which case no
    /// placement was added either.
    [[nodiscard]] bool queue_blur(PlacementGroup group, OffscreenTarget& target,
                                  BlurChain& chain, const Box& bounds, const BlurParams& params,
                                  const Box& box, float radius, int spread);

    /// As `queue_blur`, but placing one placement per region the surface tree
    /// declared through ext-background-effect-v1 — translated by the root's
    /// origin and clipped to each child's surface, `clip`, the view and
    /// `bounds`. Answers whether at least one region was placed and a capture
    /// queued: an empty client request queues nothing and therefore captures
    /// nothing.
    [[nodiscard]] bool queue_blur_regions(PlacementGroup group, OffscreenTarget& target,
                                          BlurChain& chain, const Box& bounds,
                                          const BlurParams& params, Surface& surface, int x,
                                          int y, const Box& clip, int spread);

    /// Mark the start of a window-sized group. Add the window's surface tree,
    /// its popups, and any other visual members with `place()`, then hand the
    /// marker to `compose_group()`. The original placements remain hit-testable
    /// while the group becomes one drawable texture, so input follows the
    /// window rather than the intermediate image.
    [[nodiscard]] PlacementGroup begin_group() const noexcept;

    /// Composite every placement added since `group` into `target`, then replace
    /// their pixels with one texture at `bounds`. `target` must be `bounds`
    /// wide and high in this output's logical pixels times its scale. The group
    /// is normally a window plus its subsurfaces/popups; applying `alpha` here
    /// fades that whole tree once, with no overlap seam.
    ///
    /// The intermediate submit joins the final submit through a sync_file, so
    /// neither pass waits on the CPU. The group is repainted as one box; callers
    /// cache/reuse the target and only call this when its tree changed or the
    /// effect itself needs a fresh image.
    [[nodiscard]] Status compose_group(PlacementGroup group, OffscreenTarget& target,
                                       const Box& bounds, float alpha = 1.0f);

    /// As above, but place the finished offscreen texture through `transform`.
    /// `bounds` still describes the source image and therefore the required
    /// target allocation; the transform describes only how that image appears
    /// on this output.
    [[nodiscard]] Status compose_group(PlacementGroup group, OffscreenTarget& target,
                                       const Box& bounds,
                                       const PlacementTransform& transform);

    /// As above, with a fragment shader applied once to the completed group.
    [[nodiscard]] Status compose_group(PlacementGroup group, OffscreenTarget& target,
                                       const Box& bounds,
                                       const PlacementTransform& transform,
                                       const FragmentShader& shader);

    /// This frame's list, back-to-front. Valid until the next `begin()`.
    [[nodiscard]] std::span<const Placement> placements() const noexcept;

    /// The opaque boxes of a placement, in layout coordinates. Valid until the
    /// next `begin()`.
    [[nodiscard]] std::span<const Box> opaque_of(const Placement& placement) const noexcept;

    /// Topmost placed surface accepting input at layout point (x,y), with the
    /// point in that surface's own coordinates. Honours the client's input
    /// region, so it agrees with the pixels by construction.
    [[nodiscard]] SurfaceId surface_at(double x, double y, double& sx, double& sy) const;

    /// "My model changed — work out what that costs." A window opened, closed,
    /// moved, resized or changed depth: none of that is visible to per-surface
    /// damage, but all of it IS visible in the placement list, so `submit()`
    /// recovers the damage by diffing this frame's list against the last one it
    /// drew. What the compositor still owes is the wake-up, because the diff
    /// only runs once a frame arrives and an idle output emits none.
    ///
    /// So: call this whenever the layout changes, and place the new layout on
    /// the next frame. It is cheap and idempotent — one `schedule_frame()` — and
    /// it is deliberately NOT a full repaint. Only the pixels that actually
    /// moved are drawn.
    void invalidate() noexcept;

    /// "Every pixel of this output is stale, and the list does not say why."
    /// The blunt instrument, for the things the diff cannot see: the background
    /// colour changing, or a target whose contents were invalidated from
    /// outside. A frame that was direct-scanned-out sets it internally, because
    /// the composited buffers then hold something that was never on screen.
    ///
    /// Prefer `invalidate()`. This one repaints the whole output.
    void damage_all() noexcept;

    /// This frame is part of a compositor-owned animation. Call while building
    /// each animated frame, after `begin()` and before `submit()`. Frame then
    /// repaints even when client damage is empty and asks the output for the
    /// next paced event. Stop calling it on the final frame and the output
    /// returns to its ordinary on-demand idle behaviour.
    ///
    /// Start an animation with `invalidate()` (or another change that asks for
    /// a frame), then use `FrameEvent::predicted_presentation_ns` to evaluate
    /// it at this call site. Keeping the clock at the output seam lets DRM,
    /// nested and headless backends choose their own pacing source.
    void animate() noexcept;

    /// Compose the placement list onto the output and put it on screen.
    ///
    /// In order: try to hand a fullscreen client's own buffer to the display
    /// untouched; otherwise repaint just what is stale — this frame's client
    /// damage plus the damage the buffer being drawn into still owes from when
    /// it was last on screen — and flip to it, with the display's out-fence and
    /// the clients' acquire fences threaded through the GPU so that nothing
    /// waits on the CPU.
    ///
    /// Fails only when the frame could not be put on screen at all.
    [[nodiscard]] Result<Presented> submit(Color background);

    /// The committed flip is now on screen. Call from `Output::present`, before
    /// pacing the clients: it is what lets a directly-scanned-out client have
    /// its buffer back. Frame callbacks and presentation feedback stay the
    /// compositor's job — it knows which surfaces are its own.
    void presented();

    /// Batch-send frame callbacks to every surface in this frame's placement
    /// list at one `time_ms` stamp — the `Surface::send_frame_done()` loop a
    /// present handler would otherwise write by hand. Call it from the
    /// `present` handler, and again when `submit()` answers
    /// `Presented::unchanged`, because that frame never presents. Surfaces
    /// that died since `begin()` are skipped.
    void send_frame_done(std::uint32_t time_ms) const;

    /// The frame that is on screen now, read back as tightly-packed RGBA
    /// pixels, `output.width() * output.height()` of them. This is what
    /// screencopy needs on an output whose frames otherwise never touch the
    /// CPU, and it is deliberately not done per frame — only when something
    /// actually asks. The span is valid until the next call.
    [[nodiscard]] Result<std::span<const Pixel>> read_back();

    /// True if this output can take a client buffer directly at all. False for
    /// headless and for a nested window whose parent has no linux-dmabuf.
    [[nodiscard]] bool direct_scanout_available() const noexcept;

    /// How the last `reset()` came out: 2 for double-buffered zero-copy
    /// scanout, 1 for the CPU read-back path, 0 for neither.
    [[nodiscard]] std::size_t target_count() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation

namespace luminaria {

PlacementTransform PlacementTransform::crop(float x, float y, float width,
                                            float height) const noexcept {
    PlacementTransform result = *this;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
        !std::isfinite(height)) {
        result.width_ = 0.0f;
        result.height_ = 0.0f;
        return result;
    }
    const float left = std::clamp(x, 0.0f, 1.0f);
    const float top = std::clamp(y, 0.0f, 1.0f);
    const float right = std::clamp(x + width, 0.0f, 1.0f);
    const float bottom = std::clamp(y + height, 0.0f, 1.0f);
    const float source_width = u1_ - u0_;
    const float source_height = v1_ - v0_;
    result.u0_ = u0_ + source_width * left;
    result.v0_ = v0_ + source_height * top;
    result.u1_ = u0_ + source_width * right;
    result.v1_ = v0_ + source_height * bottom;
    return result;
}

PlacementTransform PlacementTransform::relocate(float x, float y) const noexcept {
    PlacementTransform result = *this;
    result.x_ = x;
    result.y_ = y;
    return result;
}

PlacementTransform PlacementTransform::rescale(float x, float y) const noexcept {
    PlacementTransform result = *this;
    result.width_ *= x;
    result.height_ *= y;
    return result;
}

PlacementTransform PlacementTransform::opacity(float alpha) const noexcept {
    PlacementTransform result = *this;
    result.alpha_ = std::isfinite(alpha) ? std::clamp(alpha, 0.0f, 1.0f) : 0.0f;
    return result;
}

PlacementTransform PlacementTransform::rounded(float radius) const noexcept {
    PlacementTransform result = *this;
    result.radius_ = std::isfinite(radius) ? std::max(radius, 0.0f) : 0.0f;
    return result;
}

// A cached bridge from one Surface's current wl_buffer to a GPU texture. Kept
// in the GPU shell rather than Surface so the core compositor has no Vulkan
// dependency. Entries are pointer-stable because their commit handler captures
// their address; their retained identity is nevertheless generational.
struct FrSurfaceTexture {
    SurfaceId surface;
    const void* buffer = nullptr;
    std::optional<GpuTexture> texture;
    bool live = false; // dmabuf imports see new pixels; uploads are snapshots
    Output* output = nullptr;
    Signal<SurfaceCommit>::Connection committed;

    void watch(Surface& value, Output& out) {
        surface = value.id();
        output = &out;
        committed = value.commit.connect([this](SurfaceCommit&) {
            if (!live) {
                texture.reset();
                buffer = nullptr;
            }
            // A client we are showing has new content: this is the wake-up that
            // lets the output stop flipping while nothing changes. Asking for a
            // frame that then turns out to be `unchanged` costs one frame and
            // then idles again; not asking freezes the screen.
            output->schedule_frame();
        });
    }
};

// One frame's worth of "what was drawn here, and where" — everything about a
// placement that changes the pixels, in OUTPUT-local coordinates. Comparing
// this frame's list against the last one is where the damage for moving,
// opening, closing, resizing and restacking windows comes from; none of that is
// reported by any client, and asking the compositor to report it by hand is the
// bookkeeping that immediate mode exists to avoid.
//
// `texture` is compared and never dereferenced — by the time it is looked at,
// the surface it belonged to may be gone. It is in the key because a texture
// swapped at an unchanged rectangle (the compositor's own cursor image) is a
// visible change that no client damage covers.
struct FrPlacementKey {
    SurfaceId surface;
    const GpuTexture* texture = nullptr;
    std::uint64_t content = 0;
    std::uint64_t shader_id = 0;
    Box box{};
    Transform transform = Transform::normal;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    float alpha = 1.0f;
    bool draw = true;
    bool solid = false;
    Color color{};
    float corner_radius = 0.0f;
    float feather = 0.0f;

    // Member and not a hidden friend: a defaulted hidden-friend operator== in a
    // module interface ICEs gcc 16.
    [[nodiscard]] bool operator==(const FrPlacementKey& other) const noexcept {
        return surface == other.surface && texture == other.texture &&
               content == other.content && shader_id == other.shader_id &&
               box.x == other.box.x && box.y == other.box.y && box.width == other.box.width &&
               box.height == other.box.height && transform == other.transform &&
               u0 == other.u0 && v0 == other.v0 && u1 == other.u1 && v1 == other.v1 &&
               alpha == other.alpha && draw == other.draw && solid == other.solid &&
               color == other.color && corner_radius == other.corner_radius &&
               feather == other.feather;
    }
};

struct Frame::Impl {
    Output* output = nullptr;
    VulkanRenderer* renderer = nullptr;
    std::uint32_t format = 0;

    // Sized for the output's mode; rebuilt by reset().
    std::vector<ScanoutTarget> targets;
    std::vector<std::uint32_t> ids; // empty on the read-back path
    std::optional<DirectScanout> direct;
    std::size_t back = 0;
    bool animating = false;

    // Rebuilt every frame, never reallocated.
    Box view{};
    std::vector<Placement> placements;
    struct Effect {
        std::size_t placement = 0;
        const FragmentShader* shader = nullptr;
        std::uint64_t id = 0;
    };
    std::vector<Effect> effects;
    std::vector<Box> opaque_arena;        // layout coordinates
    std::vector<SurfaceAt> tree;          // scratch for place()
    std::vector<GpuTextureFill> fills;
    std::vector<const FragmentShader*> fill_shaders;
    std::vector<Box> fill_opaque;         // the same boxes, output-local
    std::vector<GpuTextureFill> group_fills;
    std::vector<Box> group_opaque;
    std::vector<int> group_acquire_fences;
    std::vector<UniqueFd> prepass_fences; // owned, joined to the final render
    std::vector<Box> prepass_damage;      // output-local, one box per group
    struct BlurRegion {
        Box box;       // output-local
        int spread = 0;
    };
    std::vector<BlurRegion> blur_regions;
    /// One deferred backdrop capture, queued while the list is built and
    /// executed by `submit()` only once the frame is known to repaint. It owns
    /// only values; `target` and `chain` are borrowed and must outlive that
    /// `submit()`. `begin()` clears every queued job.
    struct BlurJob {
        std::size_t group_first = 0; // capture everything placed since this marker
        std::size_t capture_end = 0; // ... up to this list index (queue time)
        OffscreenTarget* target = nullptr;
        BlurChain* chain = nullptr;
        Box bounds{};
        BlurParams params{};
        std::size_t placement_first = 0; // placements this job owns, for failure
        std::size_t placement_count = 0;
    };
    std::vector<BlurJob> blur_jobs;
    std::vector<SurfaceId> prepass_drawn;
    std::vector<SurfaceId> drawn;
    std::vector<std::unique_ptr<FrSurfaceTexture>> texture_cache;
    Signal<SurfaceInvalidated>::Connection surface_invalidated;
    std::vector<Box> damage;              // this frame's, output-local
    std::vector<Box> repaint;             // damage + what the target still owes
    std::vector<int> acquire_fences;      // borrowed from the surfaces
    std::vector<int> wait_fences;         // borrowed, incl. prepass_fences
    std::vector<std::uint8_t> readback;   // CPU path only
    std::vector<Pixel> frame_pixels;      // CPU path only

    // What each OTHER buffer in the rotation still owes. The target about to be
    // drawn into is `debt.size()` frames old, not one, so it has to repaint
    // every frame's damage since it was last on screen.
    std::vector<std::vector<Box>> debt;
    bool full_redraw = true;

    // The list as it was at the last submit that reached the screen, and the
    // view it was placed against. Not the last `begin()`/`place()` rebuild:
    // hit-testing rebuilds the list too, and those rebuilds draw nothing.
    std::vector<FrPlacementKey> last;
    Box last_view{};
    bool last_valid = false;
    std::uint64_t generation = 0;

    void rotate_debt();
    void diff_damage();
    void keep_list();
    [[nodiscard]] const Effect* effect_at(std::size_t placement) const noexcept;
    [[nodiscard]] FrPlacementKey key_of(const Placement& p, std::size_t placement) const noexcept;
    [[nodiscard]] GpuTexture* texture_for(Surface& surface);

    /// Composite `placements[first..]` into `target`, which covers `bounds`.
    ///
    /// `consume` is the difference between the two callers. A group is
    /// consumed: its placements stop drawing themselves because the one texture
    /// now speaks for them, and the surfaces are told their pixels have been
    /// read. A blur backdrop is not: the same placements are about to be drawn
    /// again for real, so marking their buffers as read here would release them
    /// a pass too early.
    ///
    /// The walk covers `[first, last)` of the placement list: `last` is
    /// normally the end of the list, and a deferred blur job passes its
    /// `capture_end` so its backdrop stops where its own (stale) placement
    /// begins — a capture must never sample the placement it is about to feed.
    [[nodiscard]] Status render_group(std::size_t first, std::size_t last,
                                      OffscreenTarget& target, const Box& bounds, int scale,
                                      bool consume);
};

// Damage/scissor boxes must cover every pixel a subpixel quad can touch. The
// geometry itself remains float all the way to the vertex push constants.
[[nodiscard]] Box fr_coverage(float x, float y, float width, float height) noexcept {
    const int left = static_cast<int>(std::floor(x));
    const int top = static_cast<int>(std::floor(y));
    const int right = static_cast<int>(std::ceil(x + width));
    const int bottom = static_cast<int>(std::ceil(y + height));
    return Box{left, top, right - left, bottom - top};
}

const Frame::Impl::Effect* Frame::Impl::effect_at(std::size_t placement) const noexcept {
    for (const Effect& effect : effects) {
        if (effect.placement == placement) {
            return &effect;
        }
    }
    return nullptr;
}

FrPlacementKey Frame::Impl::key_of(const Placement& p, std::size_t placement) const noexcept {
    FrPlacementKey key{};
    key.surface = p.surface;
    // Hit-test-only members of a composed group do not contribute pixels to
    // the output. Their surface identity must still be retained for input, but
    // treating their texture as drawable here would manufacture output damage
    // beside the group's one final texture.
    key.texture = p.draw ? p.texture : nullptr;
    key.content = p.draw ? p.content : 0;
    if (p.draw) {
        if (const Effect* effect = effect_at(placement); effect != nullptr) {
            key.shader_id = effect->id;
        }
    }
    // A shadow reaches past the box that casts it, and the box in the key is
    // what the damage diff repaints — so it has to be the covered area, or a
    // moved window leaves its old fringe behind on the screen.
    key.box = fr_coverage(p.x - static_cast<float>(view.x) - p.feather,
                          p.y - static_cast<float>(view.y) - p.feather,
                          p.width + 2.0f * p.feather, p.height + 2.0f * p.feather);
    key.transform = p.transform;
    key.u0 = p.u0;
    key.v0 = p.v0;
    key.u1 = p.u1;
    key.v1 = p.v1;
    key.alpha = p.alpha;
    key.draw = p.draw;
    key.solid = p.solid;
    key.corner_radius = p.corner_radius;
    key.feather = p.feather;
    key.color = p.color;
    return key;
}

void Frame::Impl::diff_damage() {
    // Walk the two lists index by index. A placement's position in the list is
    // part of its identity, because the list is z-ordered: two windows swapping
    // depth are identical field for field and still change every pixel where
    // they overlap. Comparing by index catches that, and costs one pass.
    //
    // Anything that differs damages both rectangles — where it was and where it
    // is — because both have to be repainted: one to reveal what was behind it,
    // one to draw it. A placement carrying no texture drew nothing, so that side
    // contributes no box — unless it is a solid rectangle, which drew a colour.
    const std::size_t n = std::max(placements.size(), last.size());
    for (std::size_t i = 0; i < n; ++i) {
        const bool had = i < last.size();
        const bool has = i < placements.size();
        const FrPlacementKey now = has ? key_of(placements[i], i) : FrPlacementKey{};
        if (had && has && last[i] == now) {
            continue; // unchanged: only this surface's own damage applies
        }
        if (had && (last[i].texture != nullptr || last[i].solid)) {
            damage.push_back(last[i].box);
        }
        if (has && (now.texture != nullptr || now.solid)) {
            damage.push_back(now.box);
        }
    }
}

void Frame::Impl::keep_list() {
    // `assign` over a vector that has been here before keeps its capacity, so a
    // steady-state frame does not allocate to remember itself.
    last.clear();
    for (std::size_t i = 0; i < placements.size(); ++i) {
        last.push_back(key_of(placements[i], i));
    }
    last_view = view;
    last_valid = true;
}

Status Frame::Impl::render_group(std::size_t first_placement, std::size_t last_placement,
                                 OffscreenTarget& target, const Box& bounds, int scale,
                                 bool consume) {
    group_fills.clear();
    group_opaque.clear();
    group_acquire_fences.clear();
    const std::size_t drawn_first = prepass_drawn.size();
    group_opaque.reserve(opaque_arena.size());
    for (std::size_t i = first_placement; i < last_placement; ++i) {
        Placement& p = placements[i];
        if (!p.draw) {
            // A composed group's own members are already represented by the
            // texture that replaced them, so a backdrop skips them and finds
            // that texture further down the list. A new group cannot contain
            // one: it would draw those pixels twice.
            if (consume) {
                return fail("an offscreen group cannot contain an already-composed group");
            }
            continue;
        }
        if (p.solid) {
            // Borders, panels, the bar's backing — a blur backdrop that dropped
            // them would blur the wallpaper through a window that is actually
            // covering it. They are not part of a composed group, which is one
            // window's own surfaces, so this only ever fires for a backdrop.
            if (consume) {
                continue;
            }
            GpuTextureFill solid{};
            solid.solid = true;
            solid.color = p.color;
            solid.corner_radius = p.corner_radius;
            solid.feather = p.feather;
            solid.x = p.x - static_cast<float>(bounds.x);
            solid.y = p.y - static_cast<float>(bounds.y);
            solid.w = p.width;
            solid.h = p.height;
            group_fills.push_back(solid);
            continue;
        }
        Surface* surface = surface_from_id(p.surface);
        if (p.surface.valid()) {
            p.texture = surface != nullptr ? texture_for(*surface) : nullptr;
        }
        if (p.texture == nullptr) {
            if (consume) {
                p.draw = false;
            }
            continue;
        }

        GpuTextureFill fill{};
        fill.texture = p.texture;
        fill.x = p.x - static_cast<float>(bounds.x);
        fill.y = p.y - static_cast<float>(bounds.y);
        fill.w = p.width;
        fill.h = p.height;
        fill.transform = p.transform;
        fill.u0 = p.u0;
        fill.v0 = p.v0;
        fill.u1 = p.u1;
        fill.v1 = p.v1;
        fill.alpha = p.alpha;
        fill.corner_radius = p.corner_radius;
        const std::size_t opaque_first = group_opaque.size();
        if (p.alpha == 1.0f) {
            for (const Box& b : std::span<const Box>{opaque_arena}.subspan(p.opaque_first,
                                                                           p.opaque_count)) {
                group_opaque.push_back(Box{b.x - bounds.x, b.y - bounds.y, b.width, b.height});
            }
        }
        fill.opaque = std::span<const Box>{group_opaque}.subspan(
            opaque_first, group_opaque.size() - opaque_first);
        group_fills.push_back(fill);
        if (surface != nullptr) {
            if (const int fence = surface->acquire_fence_fd(); fence >= 0) {
                group_acquire_fences.push_back(fence);
            }
            if (consume) {
                prepass_drawn.push_back(p.surface);
            }
        }
        if (consume) {
            // Keep this item for input, but only the group texture may paint it.
            p.draw = false;
        }
    }
    if (group_fills.empty()) {
        return fail("offscreen group has no drawable texture");
    }

    int fence = -1;
    const RenderSync sync{group_acquire_fences, &fence};
    const Color transparent{0, 0, 0, 0};
    if (auto rendered =
            renderer->render_offscreen(target, transparent, {}, group_fills, {}, scale, sync);
        !rendered) {
        if (fence >= 0) {
            ::close(fence);
        }
        return fail(rendered.error().message);
    }
    if (fence >= 0) {
        prepass_fences.emplace_back(fence);
    }
    for (std::size_t i = drawn_first; i < prepass_drawn.size(); ++i) {
        if (Surface* surface = surface_from_id(prepass_drawn[i]); surface != nullptr) {
            surface->notify_rendered(fence);
        }
    }
    return ok();
}

GpuTexture* Frame::Impl::texture_for(Surface& surface) {
    std::erase_if(texture_cache, [](const std::unique_ptr<FrSurfaceTexture>& entry) {
        return !entry->surface.valid();
    });

    FrSurfaceTexture* entry = nullptr;
    for (const std::unique_ptr<FrSurfaceTexture>& candidate : texture_cache) {
        if (candidate->surface == surface.id()) {
            entry = candidate.get();
            break;
        }
    }
    if (entry == nullptr) {
        auto fresh = std::make_unique<FrSurfaceTexture>();
        entry = fresh.get();
        entry->watch(surface, *output);
        texture_cache.push_back(std::move(fresh));
    }

    const void* current = surface.current_buffer_identity();
    if (current == nullptr) {
        entry->texture.reset();
        entry->buffer = nullptr;
        return nullptr;
    }
    if (entry->texture.has_value() && entry->buffer == current) {
        return &*entry->texture;
    }

    entry->texture.reset();
    entry->buffer = current;
    entry->live = false;

    if (DmabufPlane plane{}; surface.current_buffer_dmabuf(plane)) {
        auto imported = renderer->import_texture(plane);
        if (imported) {
            entry->texture.emplace(std::move(*imported));
            entry->live = true;
            return &*entry->texture;
        }
        // A dmabuf we could not import falls back to a per-frame GPU->CPU
        // readback + CPU->GPU upload — the most expensive path this library
        // has. Say so once per surface; a silent fallback is a mystery a
        // year later.
        static std::set<std::uint32_t> warned;
        if (warned.insert(surface.id().index).second) {
            std::cerr << "luminaria: dmabuf import failed for surface "
                      << surface.id().index << ": " << imported.error().message << "\n";
        }
    }

    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    if (!surface.current_buffer_rgba(rgba, width, height)) {
        entry->buffer = nullptr;
        return nullptr;
    }
    if (auto uploaded = renderer->upload_texture(width, height, rgba)) {
        entry->texture.emplace(std::move(*uploaded));
        return &*entry->texture;
    }
    entry->buffer = nullptr;
    return nullptr;
}

void Frame::Impl::rotate_debt() {
    // debt[0] is the oldest outstanding frame — the one the buffer we just drew
    // into has now paid off. Reuse its storage for this frame's damage.
    if (debt.empty()) {
        return;
    }
    std::vector<Box> recycled = std::move(debt.front());
    debt.erase(debt.begin());
    recycled.assign(damage.begin(), damage.end());
    debt.push_back(std::move(recycled));
}

Frame::Frame(Output& output, VulkanRenderer& renderer) : impl_(std::make_unique<Impl>()) {
    impl_->output = &output;
    impl_->renderer = &renderer;
    Impl* raw = impl_.get();
    impl_->surface_invalidated =
        surface_invalidated().connect([raw](SurfaceInvalidated& event) {
            for (const std::unique_ptr<FrSurfaceTexture>& entry : raw->texture_cache) {
                if (entry->surface != event.surface) {
                    continue;
                }
                entry->surface = {};
                entry->buffer = nullptr;
                entry->texture.reset();
                entry->committed.disconnect();
            }
        });
}

Frame::~Frame() = default;
Frame::Frame(Frame&&) noexcept = default;
Frame& Frame::operator=(Frame&&) noexcept = default;

std::size_t Frame::target_count() const noexcept { return impl_->targets.size(); }

bool Frame::direct_scanout_available() const noexcept {
    return impl_->direct.has_value() && impl_->direct->available();
}

Status Frame::reset(std::uint32_t drm_format) {
    Impl& impl = *impl_;
    Output& output = *impl.output;
    VulkanRenderer& renderer = *impl.renderer;
    impl.format = drm_format;

    impl.targets.clear();
    impl.ids.clear();
    impl.back = 0;
    impl.debt.clear();
    impl.full_redraw = true;
    // Whatever was on screen described the old mode; ask for the frame that
    // replaces it rather than waiting for a client to happen to redraw.
    output.schedule_frame();
    if (impl.direct.has_value()) {
        impl.direct->clear();
    } else {
        impl.direct.emplace(output);
    }

    const int w = output.width();
    const int h = output.height();

    // Zero copy first: a layout both the GPU can render into and the display can
    // scan out. Two of them, because the one on screen must not be drawn into.
    std::vector<std::uint64_t> both;
    const std::vector<std::uint64_t> shown = output.scanout_modifiers(drm_format);
    for (std::uint64_t m : renderer.scanout_modifiers(drm_format)) {
        if (std::find(shown.begin(), shown.end(), m) != shown.end()) {
            both.push_back(m);
        }
    }
    std::string why;
    while (!both.empty() && impl.targets.size() < 2) {
        auto target = renderer.create_scanout(w, h, drm_format, both);
        if (!target) {
            why = target.error().message;
            break;
        }
        auto id = output.import_scanout(target->plane());
        if (!id) {
            why = id.error().message;
            break;
        }
        impl.targets.push_back(std::move(*target));
        impl.ids.push_back(*id);
    }
    if (impl.targets.size() < 2) {
        // A single imported buffer is worse than none: it would be drawn into
        // while the display is reading it. Give back the one that did import —
        // its framebuffer would otherwise sit on the output until it dies.
        for (std::uint32_t id : impl.ids) {
            output.release_scanout(id);
        }
        impl.targets.clear();
        impl.ids.clear();
    }

    // No zero-copy path: one target still buys GPU compositing, and only the
    // finished frame crosses to the CPU (`read_scanout` + `commit_frame`).
    if (impl.targets.empty()) {
        auto target = renderer.create_scanout(w, h, drm_format, {});
        if (!target) {
            return fail(why.empty() ? target.error().message : why);
        }
        impl.targets.push_back(std::move(*target));
    }

    // With N buffers in rotation, N-1 frames of damage are outstanding against
    // the one about to be drawn into.
    impl.debt.resize(impl.targets.size() - 1);
    return ok();
}

void Frame::begin(const Box& view) {
    Impl& impl = *impl_;
    // A caller that abandoned a build after composing a group owns no fence:
    // the GPU imported it into the prepass already, so closing our sync_file
    // copy is safe. The next begun list starts with no unsubmitted group work.
    impl.prepass_fences.clear();
    impl.prepass_damage.clear();
    impl.blur_regions.clear();
    impl.blur_jobs.clear();
    impl.prepass_drawn.clear();
    impl.animating = false;
    impl.view = view;
    ++impl.generation;
    impl.placements.clear();
    impl.effects.clear();
    impl.opaque_arena.clear();
}

void Frame::place(Surface& surface, int x, int y) {
    Impl& impl = *impl_;
    impl.tree.clear();
    surface.surface_tree(impl.tree);
    for (const SurfaceAt& at : impl.tree) {
        Surface& s = *at.surface;
        const int sx = x + at.x;
        const int sy = y + at.y;
        const Box box{sx, sy, s.surface_width(), s.surface_height()};
        // Nothing of it lands on this output: not drawn, and not hit-testable
        // here either — the pointer is somewhere else entirely.
        if (box.empty() || impl.view.intersection(box).empty()) {
            continue;
        }
        Placement p{};
        p.surface = s.id();
        p.x = sx;
        p.y = sy;
        p.width = box.width;
        p.height = box.height;
        p.transform = s.buffer_transform();
        s.buffer_source_uv(p.u0, p.v0, p.u1, p.v1);
        // What the client promised is opaque, moved into layout coordinates: the
        // renderer then draws nothing at all behind it. The whole region travels
        // and not its bounding box — a window with rounded corners is opaque
        // everywhere but the corners, and claiming those would cull the
        // wallpaper that should show through them.
        p.opaque_first = static_cast<std::uint32_t>(impl.opaque_arena.size());
        for (const Box& b : s.opaque_region().rects()) {
            impl.opaque_arena.push_back(Box{b.x + sx, b.y + sy, b.width, b.height});
        }
        p.opaque_count =
            static_cast<std::uint32_t>(impl.opaque_arena.size()) - p.opaque_first;
        impl.placements.push_back(p);
    }
}

void Frame::place(Surface& surface, const PlacementTransform& transform) {
    Impl& impl = *impl_;
    if (!std::isfinite(transform.x_) || !std::isfinite(transform.y_) ||
        !std::isfinite(transform.width_) || !std::isfinite(transform.height_) ||
        !std::isfinite(transform.u0_) || !std::isfinite(transform.v0_) ||
        !std::isfinite(transform.u1_) || !std::isfinite(transform.v1_) ||
        transform.width_ <= 0.0f || transform.height_ <= 0.0f ||
        transform.u1_ <= transform.u0_ || transform.v1_ <= transform.v0_) {
        return;
    }
    const float root_width = static_cast<float>(surface.surface_width());
    const float root_height = static_cast<float>(surface.surface_height());
    if (root_width <= 0.0f || root_height <= 0.0f) {
        return;
    }
    const float crop_x0 = transform.u0_ * root_width;
    const float crop_y0 = transform.v0_ * root_height;
    const float crop_x1 = transform.u1_ * root_width;
    const float crop_y1 = transform.v1_ * root_height;
    const float crop_width = crop_x1 - crop_x0;
    const float crop_height = crop_y1 - crop_y0;
    if (crop_width <= 0.0f || crop_height <= 0.0f) {
        return;
    }
    // A bare transform moves/scales the whole tree, including a subsurface
    // which legitimately extends beyond its parent's rectangle. An explicit
    // crop, on the other hand, is defined in the root's coordinates and clips
    // the tree to that root rectangle.
    const bool full_root_crop = transform.u0_ == 0.0f && transform.v0_ == 0.0f &&
                                transform.u1_ == 1.0f && transform.v1_ == 1.0f;

    impl.tree.clear();
    surface.surface_tree(impl.tree);
    for (const SurfaceAt& at : impl.tree) {
        Surface& s = *at.surface;
        const float child_x0 = static_cast<float>(at.x);
        const float child_y0 = static_cast<float>(at.y);
        const float child_width = static_cast<float>(s.surface_width());
        const float child_height = static_cast<float>(s.surface_height());
        const float child_x1 = child_x0 + child_width;
        const float child_y1 = child_y0 + child_height;
        const float clip_x0 = full_root_crop ? child_x0 : std::max(child_x0, crop_x0);
        const float clip_y0 = full_root_crop ? child_y0 : std::max(child_y0, crop_y0);
        const float clip_x1 = full_root_crop ? child_x1 : std::min(child_x1, crop_x1);
        const float clip_y1 = full_root_crop ? child_y1 : std::min(child_y1, crop_y1);
        if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0) {
            continue;
        }

        const float input_x = clip_x0 - child_x0;
        const float input_y = clip_y0 - child_y0;
        const float input_width = clip_x1 - clip_x0;
        const float input_height = clip_y1 - clip_y0;
        Placement p{};
        p.surface = s.id();
        p.x = transform.x_ + (clip_x0 - crop_x0) * transform.width_ / crop_width;
        p.y = transform.y_ + (clip_y0 - crop_y0) * transform.height_ / crop_height;
        p.width = input_width * transform.width_ / crop_width;
        p.height = input_height * transform.height_ / crop_height;
        const Box coverage = fr_coverage(p.x, p.y, p.width, p.height);
        if (coverage.empty() || impl.view.intersection(coverage).empty()) {
            continue;
        }
        p.input_x = input_x;
        p.input_y = input_y;
        p.input_scale_x = input_width / p.width;
        p.input_scale_y = input_height / p.height;
        p.transform = s.buffer_transform();
        float source_u0 = 0.0f, source_v0 = 0.0f, source_u1 = 1.0f, source_v1 = 1.0f;
        s.buffer_source_uv(source_u0, source_v0, source_u1, source_v1);
        p.u0 = source_u0 + (source_u1 - source_u0) * input_x / child_width;
        p.v0 = source_v0 + (source_v1 - source_v0) * input_y / child_height;
        p.u1 = source_u0 + (source_u1 - source_u0) * (input_x + input_width) / child_width;
        p.v1 = source_v0 + (source_v1 - source_v0) * (input_y + input_height) / child_height;
        p.alpha = transform.alpha_;
        // Scaling an arbitrary client opaque region would need a fractional
        // region representation. Leaving it non-opaque is conservative: it
        // costs some overdraw but never hides wallpaper or another client.
        p.opaque_first = static_cast<std::uint32_t>(impl.opaque_arena.size());
        impl.placements.push_back(p);
    }
}

void Frame::place(const GpuTexture& texture, int x, int y, int width, int height) {
    place(texture, x, y, width, height, 1.0f);
}

void Frame::place(const GpuTexture& texture, int x, int y, int width, int height, float alpha) {
    place(texture, static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
          static_cast<float>(height), alpha);
}

void Frame::place(const GpuTexture& texture, float x, float y, float width, float height,
                  float alpha) {
    place(texture, PlacementTransform::at(x, y, width, height).opacity(alpha));
}

void Frame::place(const GpuTexture& texture, const PlacementTransform& transform,
                  std::uint64_t content) {
    Impl& impl = *impl_;
    if (!std::isfinite(transform.x_) || !std::isfinite(transform.y_) ||
        !std::isfinite(transform.width_) || !std::isfinite(transform.height_) ||
        !std::isfinite(transform.u0_) || !std::isfinite(transform.v0_) ||
        !std::isfinite(transform.u1_) || !std::isfinite(transform.v1_) ||
        transform.width_ <= 0.0f || transform.height_ <= 0.0f ||
        transform.u1_ <= transform.u0_ || transform.v1_ <= transform.v0_) {
        return;
    }
    const Box box = fr_coverage(transform.x_, transform.y_, transform.width_, transform.height_);
    if (box.empty() || impl.view.intersection(box).empty()) {
        return;
    }
    Placement p{};
    p.texture = &texture;
    p.x = transform.x_;
    p.y = transform.y_;
    p.width = transform.width_;
    p.height = transform.height_;
    p.u0 = transform.u0_;
    p.v0 = transform.v0_;
    p.u1 = transform.u1_;
    p.v1 = transform.v1_;
    p.alpha = transform.alpha_;
    p.corner_radius = transform.radius_;
    p.content = content;
    p.opaque_first = static_cast<std::uint32_t>(impl.opaque_arena.size());
    impl.placements.push_back(p);
}

void Frame::place(const GpuTexture& texture, const PlacementTransform& transform,
                  const FragmentShader& shader) {
    const std::size_t before = impl_->placements.size();
    place(texture, transform);
    if (impl_->placements.size() != before) {
        impl_->effects.push_back(Impl::Effect{impl_->placements.size() - 1, &shader, shader.id()});
    }
}

void Frame::place_shadow(const Box& box, Color color, float radius, float feather) {
    Impl& impl = *impl_;
    const int grow = static_cast<int>(std::ceil(std::max(feather, 0.0f)));
    if (box.empty() || grow <= 0 || color.a <= 0.0f) {
        return;
    }
    // Culled against what the shadow actually covers, not against its caster: a
    // window just off the edge of this output still casts onto it.
    const Box covered{box.x - grow, box.y - grow, box.width + 2 * grow, box.height + 2 * grow};
    if (impl.view.intersection(covered).empty()) {
        return;
    }
    Placement p{};
    p.solid = true;
    p.color = color;
    p.x = static_cast<float>(box.x);
    p.y = static_cast<float>(box.y);
    p.width = static_cast<float>(box.width);
    p.height = static_cast<float>(box.height);
    p.corner_radius = std::max(radius, 0.0f);
    p.feather = std::max(feather, 0.0f);
    impl.placements.push_back(p);
}

void Frame::place_rect(int x, int y, int width, int height, Color color) {
    Impl& impl = *impl_;
    const Box box{x, y, width, height};
    // Nothing of it lands on this output: not drawn, and not hit-testable here
    // either — the pointer is somewhere else entirely.
    if (box.empty() || impl.view.intersection(box).empty()) {
        return;
    }
    Placement p{};
    p.solid = true;
    p.color = color;
    p.x = static_cast<float>(x);
    p.y = static_cast<float>(y);
    p.width = static_cast<float>(width);
    p.height = static_cast<float>(height);
    impl.placements.push_back(p);
}

Status Frame::capture_blur(PlacementGroup group, OffscreenTarget& target, const Box& bounds,
                           BlurChain& chain, const BlurParams& params) {
    Impl& impl = *impl_;
    if (group.generation_ != impl.generation || group.first_ >= impl.placements.size()) {
        return fail("blur backdrop is empty or belongs to an earlier frame");
    }
    const int scale = std::max(1, impl.output->scale());
    if (bounds.empty() || target.width() != bounds.width * scale ||
        target.height() != bounds.height * scale) {
        return fail("blur target dimensions do not match the bounds and output scale");
    }
    if (chain.width() != target.width() || chain.height() != target.height()) {
        return fail("blur chain was not created at the target's size");
    }
    if (Status rendered = impl.render_group(group.first_, impl.placements.size(), target, bounds,
                                            scale, /*consume=*/false);
        !rendered) {
        return rendered;
    }
    return impl.renderer->blur(chain, target.texture(), params);
}

/// Add `texture` — addressed against `background` — as a blur placement over
/// `visible`, and record its damage reach. Shared by `place_blur` and the
/// placement half of `queue_blur`, so the two cannot drift.
void append_blur_placement(Frame::Impl& impl, const GpuTexture& texture, const Box& background,
                           const Box& visible, float radius, int spread) {
    Placement p{};
    p.texture = &texture;
    p.x = static_cast<float>(visible.x);
    p.y = static_cast<float>(visible.y);
    p.width = static_cast<float>(visible.width);
    p.height = static_cast<float>(visible.height);
    p.u0 = static_cast<float>(visible.x - background.x) / static_cast<float>(background.width);
    p.v0 = static_cast<float>(visible.y - background.y) / static_cast<float>(background.height);
    p.u1 = static_cast<float>(visible.x + visible.width - background.x) /
           static_cast<float>(background.width);
    p.v1 = static_cast<float>(visible.y + visible.height - background.y) /
           static_cast<float>(background.height);
    p.corner_radius = std::max(radius, 0.0f);
    p.opaque_first = static_cast<std::uint32_t>(impl.opaque_arena.size());
    impl.placements.push_back(p);
    impl.blur_regions.push_back(Frame::Impl::BlurRegion{
        Box{visible.x - impl.view.x, visible.y - impl.view.y, visible.width, visible.height},
        std::max(spread, 0)});
}

void Frame::place_blur(const GpuTexture& texture, const Box& background, const Box& box,
                       float radius, int spread) {
    Impl& impl = *impl_;
    const Box visible = box.intersection(impl.view).intersection(background);
    if (background.empty() || visible.empty()) {
        return;
    }
    append_blur_placement(impl, texture, background, visible, radius, spread);
}

bool Frame::queue_blur(PlacementGroup group, OffscreenTarget& target, BlurChain& chain,
                       const Box& bounds, const BlurParams& params, const Box& box,
                       float radius, int spread) {
    Impl& impl = *impl_;
    if (group.generation_ != impl.generation || group.first_ >= impl.placements.size()) {
        return false;
    }
    const int scale = std::max(1, impl.output->scale());
    if (bounds.empty() || target.width() != bounds.width * scale ||
        target.height() != bounds.height * scale || chain.width() != target.width() ||
        chain.height() != target.height()) {
        return false;
    }
    const Box visible = box.intersection(impl.view).intersection(bounds);
    if (visible.empty()) {
        return false;
    }
    // The capture is everything below the item that is in the list by now —
    // never the placement this call is about to add.
    const std::size_t placement_first = impl.placements.size();
    append_blur_placement(impl, chain.texture(), bounds, visible, radius, spread);
    impl.blur_jobs.push_back(Impl::BlurJob{
        .group_first = group.first_,
        .capture_end = placement_first,
        .target = &target,
        .chain = &chain,
        .bounds = bounds,
        .params = params,
        .placement_first = placement_first,
        .placement_count = impl.placements.size() - placement_first,
    });
    return true;
}

bool Frame::queue_blur_regions(PlacementGroup group, OffscreenTarget& target, BlurChain& chain,
                               const Box& bounds, const BlurParams& params, Surface& surface,
                               int x, int y, const Box& clip, int spread) {
    Impl& impl = *impl_;
    if (group.generation_ != impl.generation || group.first_ >= impl.placements.size() ||
        bounds.empty()) {
        return false;
    }
    const int scale = std::max(1, impl.output->scale());
    if (target.width() != bounds.width * scale || target.height() != bounds.height * scale ||
        chain.width() != target.width() || chain.height() != target.height()) {
        return false;
    }
    // The region walk is place_blur_regions'; it must not be written twice.
    const std::size_t placement_first = impl.placements.size();
    if (!place_blur_regions(chain.texture(), bounds, surface, x, y, clip, spread)) {
        return false; // nothing placed: an empty request captures nothing
    }
    impl.blur_jobs.push_back(Impl::BlurJob{
        .group_first = group.first_,
        .capture_end = placement_first,
        .target = &target,
        .chain = &chain,
        .bounds = bounds,
        .params = params,
        .placement_first = placement_first,
        .placement_count = impl.placements.size() - placement_first,
    });
    return true;
}

bool Frame::place_blur_regions(const GpuTexture& texture, const Box& background,
                               Surface& surface, int x, int y, const Box& clip, int spread) {
    Impl& impl = *impl_;
    if (background.empty()) {
        return false;
    }
    bool placed = false;
    impl.tree.clear();
    surface.surface_tree(impl.tree);
    for (const SurfaceAt& at : impl.tree) {
        const Surface& child = *at.surface;
        const int origin_x = x + at.x;
        const int origin_y = y + at.y;
        const Box child_box{origin_x, origin_y, child.surface_width(), child.surface_height()};
        for (const Box& local : child.blur_region().rects()) {
            const Box visible =
                Box{origin_x + local.x, origin_y + local.y, local.width, local.height}
                    .intersection(child_box)
                    .intersection(clip)
                    .intersection(impl.view)
                    .intersection(background);
            if (visible.empty()) {
                continue;
            }
            Placement p{};
            p.texture = &texture;
            p.x = static_cast<float>(visible.x);
            p.y = static_cast<float>(visible.y);
            p.width = static_cast<float>(visible.width);
            p.height = static_cast<float>(visible.height);
            p.u0 = static_cast<float>(visible.x - background.x) /
                   static_cast<float>(background.width);
            p.v0 = static_cast<float>(visible.y - background.y) /
                   static_cast<float>(background.height);
            p.u1 = static_cast<float>(visible.x + visible.width - background.x) /
                   static_cast<float>(background.width);
            p.v1 = static_cast<float>(visible.y + visible.height - background.y) /
                   static_cast<float>(background.height);
            p.opaque_first = static_cast<std::uint32_t>(impl.opaque_arena.size());
            impl.placements.push_back(p);
            impl.blur_regions.push_back(Impl::BlurRegion{
                Box{visible.x - impl.view.x, visible.y - impl.view.y, visible.width,
                    visible.height},
                std::max(spread, 0)});
            placed = true;
        }
    }
    return placed;
}

PlacementGroup Frame::begin_group() const noexcept {
    return PlacementGroup{impl_->placements.size(), impl_->generation};
}

Status Frame::compose_group(PlacementGroup group, OffscreenTarget& target, const Box& bounds,
                            float alpha) {
    return compose_group(group, target, bounds,
                         PlacementTransform::at(static_cast<float>(bounds.x),
                                                static_cast<float>(bounds.y),
                                                static_cast<float>(bounds.width),
                                                static_cast<float>(bounds.height))
                             .opacity(alpha));
}

Status Frame::compose_group(PlacementGroup group, OffscreenTarget& target, const Box& bounds,
                            const PlacementTransform& transform) {
    Impl& impl = *impl_;
    if (group.generation_ != impl.generation || group.first_ >= impl.placements.size()) {
        return fail("offscreen group is empty or belongs to an earlier frame");
    }
    const int scale = std::max(1, impl.output->scale());
    if (bounds.empty() || target.width() != bounds.width * scale ||
        target.height() != bounds.height * scale) {
        return fail("offscreen target dimensions do not match the group bounds and output scale");
    }

    if (Status rendered = impl.render_group(group.first_, impl.placements.size(), target, bounds,
                                            scale, /*consume=*/true);
        !rendered) {
        return rendered;
    }
    const std::size_t before = impl.placements.size();
    place(target.texture(), transform);
    if (impl.placements.size() != before) {
        const Placement& final = impl.placements.back();
        impl.prepass_damage.push_back(fr_coverage(final.x - static_cast<float>(impl.view.x),
                                                  final.y - static_cast<float>(impl.view.y),
                                                  final.width, final.height));
    }
    return ok();
}

Status Frame::compose_group(PlacementGroup group, OffscreenTarget& target, const Box& bounds,
                            const PlacementTransform& transform, const FragmentShader& shader) {
    const std::size_t before = impl_->placements.size();
    if (auto status = compose_group(group, target, bounds, transform); !status) {
        return status;
    }
    if (impl_->placements.size() != before) {
        impl_->effects.push_back(Impl::Effect{impl_->placements.size() - 1, &shader, shader.id()});
    }
    return ok();
}

std::span<const Placement> Frame::placements() const noexcept { return impl_->placements; }

std::span<const Box> Frame::opaque_of(const Placement& placement) const noexcept {
    return std::span<const Box>{impl_->opaque_arena}.subspan(placement.opaque_first,
                                                             placement.opaque_count);
}

SurfaceId Frame::surface_at(double x, double y, double& sx, double& sy) const {
    const std::vector<Placement>& list = impl_->placements;
    for (auto it = list.rbegin(); it != list.rend(); ++it) { // topmost first
        Surface* surface = surface_from_id(it->surface);
        if (surface == nullptr) {
            continue;
        }
        const double lx = it->input_x + (x - it->x) * it->input_scale_x;
        const double ly = it->input_y + (y - it->y) * it->input_scale_y;
        if (surface->accepts_input(lx, ly)) {
            sx = lx;
            sy = ly;
            return it->surface;
        }
    }
    return {};
}

void Frame::invalidate() noexcept {
    // No damage is recorded here on purpose. The layout the compositor just
    // changed is not in this frame yet — it arrives with the next `begin()` /
    // `place()` — and `submit()` will find it by diffing. All that is owed now
    // is the wake-up, because an output nobody is committing to emits no frames
    // and would otherwise sit on the old picture forever.
    impl_->output->schedule_frame();
}

void Frame::damage_all() noexcept {
    impl_->full_redraw = true;
    impl_->output->schedule_frame();
}

void Frame::animate() noexcept { impl_->animating = true; }

Result<std::span<const Pixel>> Frame::read_back() {
    Impl& impl = *impl_;
    if (impl.targets.empty()) {
        return fail("this frame has no render target to read back");
    }
    // On the read-back path the pixels are already here — they are what was
    // committed. On the zero-copy path `back` points at the target for the NEXT
    // frame, so the one before it is what the display is showing.
    if (!impl.ids.empty()) {
        const std::size_t shown = (impl.back + impl.targets.size() - 1) % impl.targets.size();
        if (auto s = impl.renderer->read_scanout(impl.targets[shown], impl.readback); !s) {
            return fail(s.error().message);
        }
        impl.frame_pixels.resize(impl.readback.size() / 4);
        std::memcpy(impl.frame_pixels.data(), impl.readback.data(), impl.readback.size());
    }
    return std::span<const Pixel>{impl.frame_pixels};
}

void Frame::presented() {
    if (impl_->direct.has_value()) {
        impl_->direct->presented();
    }
}

void Frame::send_frame_done(std::uint32_t time_ms) const {
    for (const Placement& p : impl_->placements) {
        if (Surface* surface = surface_from_id(p.surface); surface != nullptr) {
            surface->send_frame_done(time_ms);
        }
    }
}

Result<Presented> Frame::submit(Color background) {
    Impl& impl = *impl_;
    Output& output = *impl.output;

    // A shader is opaque to the placement diff: if it says its result changes
    // beyond ordinary texture/client damage, make that cost explicit before
    // considering direct scanout or deciding this frame is unchanged.
    for (std::size_t i = 0; i < impl.placements.size(); ++i) {
        const Placement& p = impl.placements[i];
        const Impl::Effect* effect = impl.effect_at(i);
        if (!p.draw || effect == nullptr || effect->shader == nullptr) {
            continue;
        }
        switch (effect->shader->damage()) {
        case ShaderDamage::none:
            break;
        case ShaderDamage::full:
            impl.full_redraw = true;
            break;
        case ShaderDamage::continuous:
            impl.full_redraw = true;
            impl.animating = true;
            break;
        }
    }

    // --- direct scanout ------------------------------------------------------
    //
    // One window, covering the whole monitor, handing us a buffer the display
    // can already read: point the CRTC at it and draw nothing at all. That is
    // the fullscreen video / game case, and it saves the entire composite pass
    // plus the bandwidth of a full-screen blit every frame.
    //
    // The cursor has to be on its own plane, or it would not appear — nothing is
    // compositing it in.
    if (!impl.animating && impl.direct.has_value() && output.has_cursor_plane() && impl.placements.size() == 1 &&
        impl.placements[0].draw && impl.placements[0].surface.valid() &&
        impl.effect_at(0) == nullptr) {
        const Placement& only = impl.placements[0];
        if (only.x == impl.view.x && only.y == impl.view.y &&
            only.width == output.logical_width() && only.height == output.logical_height() &&
            only.transform == Transform::normal && only.u0 == 0.0f && only.v0 == 0.0f &&
            only.u1 == 1.0f && only.v1 == 1.0f && only.alpha == 1.0f) {
            Surface* surface = surface_from_id(only.surface);
            if (surface != nullptr) {
                if (auto id = impl.direct->id_for(*surface)) {
                    // The client's own acquire fence goes straight to KMS: the
                    // flip waits for the client's GPU work and we never touch it.
                    int fence = surface->acquire_fence_fd();
                    if (fence >= 0) {
                        fence = ::dup(fence);
                    }
                    if (auto s = output.commit_scanout(*id, fence)) {
                        impl.direct->committed(*surface);
                        surface->clear_damage();
                        // The composited buffers no longer hold what is on screen,
                        // so the next composited frame must be a complete one.
                        impl.full_redraw = true;
                        // Nothing was composited, so there is no list to diff
                        // the next one against — and importing this buffer just
                        // to record it would undo the saving direct scanout is
                        // here for. The full redraw above covers it.
                        impl.last_valid = false;
                        return Presented::scanout;
                    }
                }
            }
        }
    }

    // --- what to draw --------------------------------------------------------
    //
    // Textures are cached in this frame's GPU bridge and owned by the renderer;
    // this is a list of borrowed pointers, and an unchanged buffer costs nothing.
    impl.fills.clear();
    impl.fill_shaders.clear();
    impl.drawn.clear();
    impl.damage.clear();
    // A composed group has already rendered its source tree into its private
    // target. Its final texture is the only output placement, but any new
    // pixels in that target make the group's whole destination stale.
    impl.damage.insert(impl.damage.end(), impl.prepass_damage.begin(), impl.prepass_damage.end());
    // The diff below compares against boxes recorded in this output's own
    // coordinates. A view that has moved or resized makes every one of them
    // describe somewhere else, so there is nothing to compare against.
    if (!impl.last_valid || impl.view.x != impl.last_view.x || impl.view.y != impl.last_view.y ||
        impl.view.width != impl.last_view.width || impl.view.height != impl.last_view.height) {
        impl.full_redraw = true;
    }
    // A forced redraw is not only about the target selected this frame. Every
    // other buffer in the rotation still contains the old scene and must carry
    // a full-output debt until it comes round. Without recording this box,
    // closing a window alternates between the cleared target and a stale target
    // for one cycle — seen as the destroyed rectangle flickering back.
    if (impl.full_redraw || impl.animating) {
        impl.damage.push_back(
            Box{0, 0, output.logical_width(), output.logical_height()});
    }
    impl.acquire_fences.clear();
    impl.fill_opaque.clear();
    // The fills hold spans into this, so it must not reallocate while they are
    // being built. It can hold at most what the arena holds, and reserving what
    // it already has capacity for allocates nothing.
    impl.fill_opaque.reserve(impl.opaque_arena.size());
    bool wants_tearing = false;
    for (std::size_t placement_index = 0; placement_index < impl.placements.size();
         ++placement_index) {
        Placement& p = impl.placements[placement_index];
        if (!p.draw) {
            continue;
        }
        Surface* surface = surface_from_id(p.surface);
        if (p.surface.valid()) {
            // This was a client placement. A non-resolving id means the client
            // destroyed it after begin(); do not retain a texture pointer from
            // an earlier submit of the same placement list.
            p.texture = surface != nullptr ? impl.texture_for(*surface) : nullptr;
        }
        if (p.solid) {
            // Compositor-owned rectangle: a fill with no texture, drawn in list
            // order like any other placement.
            GpuTextureFill fill{};
            fill.solid = true;
            fill.color = p.color;
            fill.corner_radius = p.corner_radius;
            fill.feather = p.feather;
            fill.x = p.x - impl.view.x;
            fill.y = p.y - impl.view.y;
            fill.w = p.width;
            fill.h = p.height;
            impl.fills.push_back(fill);
            impl.fill_shaders.push_back(nullptr);
            continue;
        }
        if (p.texture == nullptr) {
            continue;
        }
        // Placements are in layout coordinates; the target is one output.
        const int x = p.x - impl.view.x;
        const int y = p.y - impl.view.y;
        if (surface != nullptr) {
            for (const Box& d : surface->damage()) {
                impl.damage.push_back(Box{x + d.x, y + d.y, d.width, d.height});
            }
            wants_tearing = wants_tearing || surface->tearing_hint();
            if (const int fence = surface->acquire_fence_fd(); fence >= 0) {
                impl.acquire_fences.push_back(fence);
            }
            impl.drawn.push_back(p.surface);
        }
        GpuTextureFill fill{};
        fill.texture = p.texture;
        fill.x = x;
        fill.y = y;
        fill.w = p.width;
        fill.h = p.height;
        fill.transform = p.transform;
        fill.u0 = p.u0;
        fill.v0 = p.v0;
        fill.u1 = p.u1;
        fill.v1 = p.v1;
        fill.alpha = p.alpha;
        fill.corner_radius = p.corner_radius;
        // The placement's opaque region, moved from layout coordinates into
        // this output's. Borrowed by the fill, so it goes into its own buffer:
        // the arena is what `opaque_of()` still answers from.
        const std::size_t first = impl.fill_opaque.size();
        if (p.alpha == 1.0f) {
            for (const Box& b : opaque_of(p)) {
                impl.fill_opaque.push_back(Box{b.x - impl.view.x, b.y - impl.view.y, b.width,
                                               b.height});
            }
        }
        fill.opaque =
            std::span<const Box>{impl.fill_opaque}.subspan(first, impl.fill_opaque.size() - first);
        impl.fills.push_back(fill);
        const Impl::Effect* effect = impl.effect_at(placement_index);
        impl.fill_shaders.push_back(effect != nullptr ? effect->shader : nullptr);
    }
    output.set_tearing(wants_tearing);

    // What no client reported, and what the compositor no longer has to: the
    // windows that opened, closed, moved, resized or changed depth since the
    // last frame that reached the screen.
    if (!impl.full_redraw) {
        impl.diff_damage();
        // A blurred region shows what is behind it from `spread` pixels away,
        // so a change that far outside it makes it stale even though nothing
        // inside it moved. Only what the frame already reported is consulted,
        // so a frame with no damage at all still adds none — an idle desktop
        // with blur on stays idle.
        const std::size_t reported = impl.damage.size();
        for (const Impl::BlurRegion& region : impl.blur_regions) {
            const Box reach{region.box.x - region.spread, region.box.y - region.spread,
                            region.box.width + 2 * region.spread,
                            region.box.height + 2 * region.spread};
            for (std::size_t i = 0; i < reported; ++i) {
                if (!impl.damage[i].intersection(reach).empty()) {
                    impl.damage.push_back(region.box);
                    break;
                }
            }
        }
    }

    // --- how much of it -----------------------------------------------------
    //
    // An empty repaint list means "everything"; one empty box means "nothing at
    // all changed", which the renderer answers by leaving the target alone.
    std::vector<Box>& repaint = impl.repaint;
    repaint.clear();
    if (!impl.full_redraw) {
        repaint.assign(impl.damage.begin(), impl.damage.end());
        for (const std::vector<Box>& owed : impl.debt) {
            repaint.insert(repaint.end(), owed.begin(), owed.end());
        }
        if (repaint.empty()) {
            // Not one pixel of this output differs from what the display is
            // already showing. Rendering it would produce the frame that is
            // there, and flipping to it would wake the display engine to
            // exchange two identical buffers — so do neither, and let the
            // output go idle until someone asks for a frame again.
            //
            // Nothing is spent here: no buffer was drawn into, so `back` does
            // not advance and no debt is rotated. The clients' damage is
            // likewise untouched, because there was none.
            //
            // No `present` will follow this frame. Anything the compositor does
            // there — frame callbacks above all — it has to do here instead, or
            // a client that committed without damaging waits forever.
            //
            // Explicit-sync clients need the same care: their release point is
            // signalled by the render, and there is no render. Nothing here
            // ever read those buffers, so they are free immediately — say so,
            // rather than leaving a syncobj client waiting on a fence that will
            // never exist.
            for (SurfaceId id : impl.drawn) {
                if (Surface* surface = surface_from_id(id); surface != nullptr) {
                    surface->notify_rendered(-1);
                }
            }
            // The list is unchanged by construction — that is what an empty
            // repaint means — so this only refreshes the resolved textures.
            impl.keep_list();
            return Presented::unchanged;
        }
    }

    // --- deferred blur captures ----------------------------------------------
    //
    // The captures were queued while the list was built; they run here because
    // they are GPU work and an unchanged frame must not pay for it. They
    // execute in queue order, so each capture's backdrop includes the previous
    // job's just-updated chain texture — the same content the synchronous path
    // would have rendered at that point in the list. Their out-fences land in
    // `prepass_fences` and join the final submission below.
    for (Impl::BlurJob& job : impl.blur_jobs) {
        const int scale = std::max(1, output.scale());
        Status status = impl.render_group(job.group_first, job.capture_end, *job.target,
                                          job.bounds, scale, /*consume=*/false);
        if (status) {
            status = impl.renderer->blur(*job.chain, job.target->texture(), job.params);
        }
        if (!status) {
            // One blur failing costs that blur, not the frame: drop the
            // placements it owns and let the rest of the scene draw. They were
            // never sampled, so the diff will repaint their boxes next frame.
            for (std::size_t i = job.placement_first;
                 i < job.placement_first + job.placement_count && i < impl.placements.size();
                 ++i) {
                impl.placements[i].draw = false;
            }
        }
    }

    Presented result = Presented::fallback;
    if (!impl.targets.empty()) {
        ScanoutTarget& target = impl.targets[impl.back];
        const OutputMapping mapping{output.transform(), output.scale()};
        impl.wait_fences.assign(impl.acquire_fences.begin(), impl.acquire_fences.end());
        for (const UniqueFd& fence : impl.prepass_fences) {
            if (fence.get() >= 0) {
                impl.wait_fences.push_back(fence.get());
            }
        }
        if (!impl.ids.empty()) {
            // The buffer about to be drawn into may still be on screen; the
            // flip's out-fence says when it is not. Waiting for that on the GPU
            // is the point — nothing here blocks.
            target.set_acquire_fence(output.take_present_fence());
            int render_fence = -1;
            const RenderSync sync{impl.wait_fences, &render_fence};
            auto rendered =
                impl.renderer->render_to_with_shaders(target, background, {}, impl.fills,
                                                      impl.fill_shaders, repaint, mapping, sync);
            // render_to imported every wait fd before returning. This Frame-owned
            // subset may now close even though the output's render itself is
            // still in flight.
            impl.prepass_fences.clear();
            if (!rendered) {
                if (render_fence >= 0) {
                    ::close(render_fence);
                }
                return fail(rendered.error().message);
            }
            // Explicit-sync clients get the render's own fence as their release
            // point: they may reuse the buffer the moment the GPU stops reading
            // it, not whenever we next get round to saying so.
            for (SurfaceId id : impl.drawn) {
                if (Surface* surface = surface_from_id(id); surface != nullptr) {
                    surface->notify_rendered(render_fence);
                }
            }
            // commit_scanout takes the fence: KMS holds the flip until the GPU
            // signals, so we never wait for the render either.
            auto flipped = output.commit_scanout(impl.ids[impl.back], render_fence);
            if (!flipped) {
                return fail(flipped.error().message);
            }
            impl.back = (impl.back + 1) % impl.targets.size();
            result = Presented::composited;
        } else {
            // No zero-copy path. The composite still happens on the GPU; only
            // the finished frame crosses to the CPU, once.
            const RenderSync sync{impl.wait_fences};
            auto rendered = impl.renderer->render_to_with_shaders(target, background, {}, impl.fills,
                                                                   impl.fill_shaders, repaint, mapping,
                                                                   sync);
            impl.prepass_fences.clear();
            if (!rendered) {
                return fail(rendered.error().message);
            }
            if (auto read = impl.renderer->read_scanout(target, impl.readback); !read) {
                return fail(read.error().message);
            }
            impl.frame_pixels.resize(impl.readback.size() / 4);
            std::memcpy(impl.frame_pixels.data(), impl.readback.data(), impl.readback.size());
            if (auto s = output.commit_frame(impl.frame_pixels, output.width(), output.height());
                !s) {
                return fail(s.error().message);
            }
            result = Presented::composited;
        }
    } else if (auto s = output.commit(background); !s) {
        return fail(s.error().message);
    }

    // The damage is spent: hand this frame's share to the buffer that will come
    // round again, and start the clients accumulating afresh.
    impl.rotate_debt();
    impl.full_redraw = false;
    // Remember what reached the screen, so the next frame can tell what moved.
    // Deliberately not done on the failure paths above: a frame that never got
    // there must be diffed against again, or its damage is simply lost.
    impl.keep_list();
    for (SurfaceId id : impl.drawn) {
        if (Surface* surface = surface_from_id(id); surface != nullptr) {
            surface->clear_damage();
        }
    }
    for (SurfaceId id : impl.prepass_drawn) {
        if (Surface* surface = surface_from_id(id); surface != nullptr) {
            surface->clear_damage();
        }
    }
    impl.prepass_damage.clear();
    impl.blur_regions.clear();
    impl.prepass_drawn.clear();
    if (impl.animating) {
        // Committing does not buy another frame. Keep this request inside the
        // shell layer so animation callers cannot accidentally freeze at the
        // first successful flip.
        output.schedule_frame();
    }
    return result;
}

} // namespace luminaria
