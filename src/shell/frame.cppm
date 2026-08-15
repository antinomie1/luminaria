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

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <unistd.h> // dup, close — the fences handed between GPU and display

export module luminaria.gpu:frame;

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

    int x = 0, y = 0;          ///< top-left, layout coordinates
    int width = 0, height = 0; ///< surface coordinates, not buffer pixels

    Transform transform = Transform::normal;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;

    /// The opaque region, in layout coordinates, as `[first, first+count)` in
    /// the frame's arena — ask `Frame::opaque_of()`. It lives here as a pair of
    /// indices rather than a `Region` because a `Region` per surface per frame
    /// is a heap allocation per surface per frame, which is precisely the cost
    /// this layer exists to not pay.
    std::uint32_t opaque_first = 0;
    std::uint32_t opaque_count = 0;
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

    /// Place a texture the compositor owns — a themed cursor on a backend with
    /// no cursor plane. It is not hit-testable and reports no damage, so a frame
    /// that only moved it needs `damage_all()`.
    void place(const GpuTexture& texture, int x, int y, int width, int height);

    /// This frame's list, back-to-front. Valid until the next `begin()`.
    [[nodiscard]] std::span<const Placement> placements() const noexcept;

    /// The opaque boxes of a placement, in layout coordinates. Valid until the
    /// next `begin()`.
    [[nodiscard]] std::span<const Box> opaque_of(const Placement& placement) const noexcept;

    /// Topmost placed surface accepting input at layout point (x,y), with the
    /// point in that surface's own coordinates. Honours the client's input
    /// region, so it agrees with the pixels by construction.
    [[nodiscard]] SurfaceId surface_at(double x, double y, double& sx, double& sy) const;

    /// "Pixels changed that no client reported." A window appearing, vanishing
    /// or moving is invisible to per-surface damage, and so is a frame that was
    /// direct-scanned-out; every one of those has to force a full repaint.
    ///
    /// It also asks the output for a frame, so this is what wakes a screen that
    /// has gone idle. Clients redrawing wake it by themselves.
    void damage_all() noexcept;

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

struct Frame::Impl {
    Output* output = nullptr;
    VulkanRenderer* renderer = nullptr;
    std::uint32_t format = 0;

    // Sized for the output's mode; rebuilt by reset().
    std::vector<ScanoutTarget> targets;
    std::vector<std::uint32_t> ids; // empty on the read-back path
    std::optional<DirectScanout> direct;
    std::size_t back = 0;

    // Rebuilt every frame, never reallocated.
    Box view{};
    std::vector<Placement> placements;
    std::vector<Box> opaque_arena;        // layout coordinates
    std::vector<SurfaceAt> tree;          // scratch for place()
    std::vector<GpuTextureFill> fills;
    std::vector<Box> fill_opaque;         // the same boxes, output-local
    std::vector<SurfaceId> drawn;
    std::vector<std::unique_ptr<FrSurfaceTexture>> texture_cache;
    Signal<SurfaceInvalidated>::Connection surface_invalidated;
    std::vector<Box> damage;              // this frame's, output-local
    std::vector<Box> repaint;             // damage + what the target still owes
    std::vector<int> acquire_fences;      // borrowed from the surfaces
    std::vector<std::uint8_t> readback;   // CPU path only
    std::vector<Pixel> frame_pixels;      // CPU path only

    // What each OTHER buffer in the rotation still owes. The target about to be
    // drawn into is `debt.size()` frames old, not one, so it has to repaint
    // every frame's damage since it was last on screen.
    std::vector<std::vector<Box>> debt;
    bool full_redraw = true;

    void rotate_debt();
    [[nodiscard]] GpuTexture* texture_for(Surface& surface);
};

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

    const void* current = surface.current_buffer();
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
        if (auto imported = renderer->import_texture(plane)) {
            entry->texture.emplace(std::move(*imported));
            entry->live = true;
            return &*entry->texture;
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
    impl.view = view;
    impl.placements.clear();
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

void Frame::place(const GpuTexture& texture, int x, int y, int width, int height) {
    Impl& impl = *impl_;
    const Box box{x, y, width, height};
    if (box.empty() || impl.view.intersection(box).empty()) {
        return;
    }
    Placement p{};
    p.texture = &texture;
    p.x = x;
    p.y = y;
    p.width = width;
    p.height = height;
    p.opaque_first = static_cast<std::uint32_t>(impl.opaque_arena.size());
    impl.placements.push_back(p);
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
        const double lx = x - it->x;
        const double ly = y - it->y;
        if (surface->accepts_input(lx, ly)) {
            sx = lx;
            sy = ly;
            return it->surface;
        }
    }
    return {};
}

void Frame::damage_all() noexcept {
    impl_->full_redraw = true;
    // Whatever moved, appeared or vanished, no client is going to report it —
    // so this is also the only chance to wake an output that has gone idle.
    impl_->output->schedule_frame();
}

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

Result<Presented> Frame::submit(Color background) {
    Impl& impl = *impl_;
    Output& output = *impl.output;

    // --- direct scanout ------------------------------------------------------
    //
    // One window, covering the whole monitor, handing us a buffer the display
    // can already read: point the CRTC at it and draw nothing at all. That is
    // the fullscreen video / game case, and it saves the entire composite pass
    // plus the bandwidth of a full-screen blit every frame.
    //
    // The cursor has to be on its own plane, or it would not appear — nothing is
    // compositing it in.
    if (impl.direct.has_value() && output.has_cursor_plane() && impl.placements.size() == 1 &&
        impl.placements[0].surface.valid()) {
        const Placement& only = impl.placements[0];
        if (only.x == impl.view.x && only.y == impl.view.y &&
            only.width == output.logical_width() && only.height == output.logical_height()) {
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
    impl.drawn.clear();
    impl.damage.clear();
    // A forced redraw is not only about the target selected this frame. Every
    // other buffer in the rotation still contains the old scene and must carry
    // a full-output debt until it comes round. Without recording this box,
    // closing a window alternates between the cleared target and a stale target
    // for one cycle — seen as the destroyed rectangle flickering back.
    if (impl.full_redraw) {
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
    for (Placement& p : impl.placements) {
        Surface* surface = surface_from_id(p.surface);
        if (p.surface.valid()) {
            // This was a client placement. A non-resolving id means the client
            // destroyed it after begin(); do not retain a texture pointer from
            // an earlier submit of the same placement list.
            p.texture = surface != nullptr ? impl.texture_for(*surface) : nullptr;
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
        // The placement's opaque region, moved from layout coordinates into
        // this output's. Borrowed by the fill, so it goes into its own buffer:
        // the arena is what `opaque_of()` still answers from.
        const std::size_t first = impl.fill_opaque.size();
        for (const Box& b : opaque_of(p)) {
            impl.fill_opaque.push_back(Box{b.x - impl.view.x, b.y - impl.view.y, b.width,
                                           b.height});
        }
        fill.opaque =
            std::span<const Box>{impl.fill_opaque}.subspan(first, impl.fill_opaque.size() - first);
        impl.fills.push_back(fill);
    }
    output.set_tearing(wants_tearing);

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
            return Presented::unchanged;
        }
    }

    Presented result = Presented::fallback;
    if (!impl.targets.empty()) {
        ScanoutTarget& target = impl.targets[impl.back];
        const OutputMapping mapping{output.transform(), output.scale()};
        if (!impl.ids.empty()) {
            // The buffer about to be drawn into may still be on screen; the
            // flip's out-fence says when it is not. Waiting for that on the GPU
            // is the point — nothing here blocks.
            target.set_acquire_fence(output.take_present_fence());
            int render_fence = -1;
            const RenderSync sync{impl.acquire_fences, &render_fence};
            auto rendered =
                impl.renderer->render_to(target, background, {}, impl.fills, repaint, mapping, sync);
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
            auto rendered =
                impl.renderer->render_to(target, background, {}, impl.fills, repaint, mapping);
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
    for (SurfaceId id : impl.drawn) {
        if (Surface* surface = surface_from_id(id); surface != nullptr) {
            surface->clear_damage();
        }
    }
    return result;
}

} // namespace luminaria
