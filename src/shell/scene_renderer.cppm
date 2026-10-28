// luminaria/shell/scene_renderer.cppm — a scene list on whatever hardware there is.
//
// `SceneItem` (`luminaria:scene`) is the list; this is the half that turns it
// into pixels. It tries `Frame` on Vulkan and falls back to `CpuCompositor`,
// and the three ways that fallback happens are all runtime, never a build
// option: no Vulkan device at all, an output whose render targets could not be
// allocated, and `Presented::fallback` from a submit that found no target.
//
// The point of the type is that the compositor above it writes each primitive
// once. Borders, solid rectangles, compositor-owned images, client surface
// trees and the cursor were all previously two implementations that had to be
// kept in step by hand, in every compositor built on this library.
//
// It also answers the display for the compositor. Once `reset()` has bound an
// output, that output's `present` event releases the frame callbacks of
// everything the frame drew and sends its presentation feedback, a frame that
// turns out `unchanged` releases them on the spot (no `present` follows it),
// and on the CPU path a commit to anything drawn asks for the next frame, as
// `Frame` does on the GPU. Those three were a contract every compositor had to
// remember, and forgetting any one froze its clients.
//
// A compositor that wants the GPU path only can still drive `Frame` directly;
// this is for the ones that must also come up on a machine with no GPU — which
// includes every test suite that runs in CI.
export module luminaria.shell:scene_renderer;

import std;

import luminaria;
import luminaria.render;

import :frame;
import :scene;

export namespace luminaria {

/// What `SceneRenderer::present()` did, for a compositor that keeps count.
/// `unchanged`: nothing was committed, so no `present` event follows it; the
/// frame callbacks it owed have already been released.
enum class SceneOutcome {
    /// Composited and flipped on the GPU.
    composited,
    /// A client's own buffer went straight to the display.
    scanout,
    /// The frame was identical to the one on screen.
    unchanged,
    /// Composited on the CPU and handed over as pixels.
    cpu,
};

/// How many frames ended each way since the renderer was made, for a
/// compositor that wants to see whether an idle screen really goes quiet.
struct SceneStats {
    std::uint64_t composited = 0, scanout = 0, unchanged = 0, cpu = 0;

    [[nodiscard]] bool operator==(const SceneStats&) const noexcept = default;
};

/// The pointer as `output` should show it, in that output's logical
/// coordinates; an empty `SceneCursor` for none. `Cursor` installs one.
using SceneCursorSource = std::function<SceneCursor(const Output& output)>;

/// Per-output renderer state: the GPU frame ledger when there is one, the CPU
/// compositor when there is not, and the scratch both reuse so that a
/// steady-state frame allocates nothing.
///
/// `SceneRenderer::reset()` binds it to an output, and from then on it answers
/// that output's `present` events by itself. It unbinds when the output is
/// destroyed or when it is itself destroyed, whichever comes first, so dropping
/// it in an `Output::destroy` handler is as safe as never dropping it.
///
/// Move-only, and it must not outlive the `SceneRenderer` that filled it.
class SceneOutput {
public:
    SceneOutput();
    ~SceneOutput();
    SceneOutput(SceneOutput&&) noexcept;
    SceneOutput& operator=(SceneOutput&&) noexcept;
    SceneOutput(const SceneOutput&) = delete;
    SceneOutput& operator=(const SceneOutput&) = delete;

    /// Whether this output is currently on the GPU path.
    [[nodiscard]] bool gpu_output() const noexcept;

    struct Impl;

private:
    friend class SceneRenderer;
    std::unique_ptr<Impl> impl_;
};

/// One per compositor. Owns the Vulkan renderer when there is one.
///
/// Constructing it never fails: a machine with no usable Vulkan device gets a
/// renderer that draws every output on the CPU, and says why in `gpu_error()`.
/// Refusing to start is not an option a compositor has.
class SceneRenderer {
public:
    SceneRenderer();
    /// As above, with the options the Vulkan renderer is created with — the
    /// DRM device to prefer, on a bare-metal compositor.
    explicit SceneRenderer(const VulkanOptions& options);
    ~SceneRenderer();
    SceneRenderer(SceneRenderer&&) noexcept;
    SceneRenderer& operator=(SceneRenderer&&) noexcept;
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    [[nodiscard]] bool gpu_enabled() const noexcept;
    /// Borrowed, null when there is none — for a compositor that also wants to
    /// advertise `linux-dmabuf`, or to upload textures of its own.
    [[nodiscard]] VulkanRenderer* gpu() noexcept;
    /// Why there is none. Empty when there is.
    [[nodiscard]] std::string_view gpu_error() const noexcept;

    /// Where presentation feedback goes. Null (the default) sends none; frame
    /// callbacks are released either way. Borrowed: it must outlive every
    /// `present` event of a bound output — declare the backends after it, which
    /// the globals-before-backends order already does.
    void set_presentation(Presentation* presentation) noexcept;

    /// Allocate everything sized for `output`'s CURRENT mode in `drm_format`
    /// (XRGB8888 unless told otherwise), and bind `state` to it. Call it once
    /// per output: from then on a `mode_changed` on that output reallocates by
    /// itself, before any handler of the compositor's own runs a frame.
    ///
    /// A failure is not fatal and is not an error the caller has to handle
    /// beyond reporting it: this output falls back to the CPU path, which needs
    /// no setup. There is nothing to allocate when there is no GPU at all, and
    /// that answers `ok()` — the binding happens either way.
    Status reset(SceneOutput& state, Output& output,
                 std::uint32_t drm_format = 0x34325258 /* DRM_FORMAT_XRGB8888 */);

    /// Draw `scene` and present it on `output`, with the pointer from the
    /// cursor source composited on top unless the cursor plane carries it.
    ///
    /// The scene is in the output's *logical* coordinates. On the GPU path
    /// `Frame` applies the output's scale and transform; the CPU path draws
    /// each surface tree at its logical size with the clip carried in device
    /// coordinates — byte-exact for scale=1 and a normal transform, which is
    /// what every software output currently is.
    [[nodiscard]] Result<SceneOutcome> present(SceneOutput& state, Output& output,
                                               std::span<const SceneItem> scene, Color background);

    /// The same, from a scene in `layout`'s coordinates — one list for every
    /// output, each showing the part `layout` puts it on. An output that is not
    /// in the layout shows the scene from the origin.
    [[nodiscard]] Result<SceneOutcome> present(SceneOutput& state, Output& output,
                                               std::span<const SceneItem> scene, Color background,
                                               const OutputLayout& layout);

    /// What the pointer looks like on each output. Without one no cursor is
    /// drawn. `Cursor` installs itself here; an empty function uninstalls.
    void set_cursor_source(SceneCursorSource source);

    /// The pointer moved or changed picture: every bound output re-reads the
    /// cursor source. One with a KMS cursor plane that takes the image moves
    /// the plane — one small atomic commit and no repaint, which is what makes
    /// moving the mouse cheap — and every other output asks for a frame, since
    /// a composited pointer is damage no client reports. An output whose plane
    /// rejects the image (too large, wrong format) silently goes back to being
    /// composited, and the pointer is never drawn twice.
    void update_cursor();

    /// Read back the last presented frame on `state` as RGBA8 pixels.
    /// On the GPU path reads back `Frame::read_back()`; on the CPU path returns
    /// `CpuCompositor::pixels()`.
    [[nodiscard]] Result<std::span<const Pixel>> read_back(SceneOutput& state);

    /// Frames `present()` has finished so far, by outcome.
    [[nodiscard]] SceneStats stats() const noexcept;

    struct Impl;

private:
    Result<SceneOutcome> draw(SceneOutput& state, Output& output, std::span<const SceneItem> scene,
                              Color background);

    std::unique_ptr<Impl> impl_;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

/// One compositor-owned image's GPU copy, kept across frames. Keyed by the
/// item's serial, so re-uploading a bar-sized texture every frame — precisely
/// the idle work an idle compositor must not do — costs one integer compare.
struct SceneImageCache {
    std::optional<GpuTexture> texture;
    std::uint64_t serial = 0;
};

/// One window-sized offscreen, kept across frames. A window that fades or
/// rounds has to be composited before the effect is applied to it (ADR 0005),
/// and reallocating that target every frame is the allocation an idle desktop
/// must not make.
struct SceneGroupCache {
    std::optional<OffscreenTarget> target;
    int width = 0, height = 0;
};

/// One blurred item's backdrop + blur chain, kept across frames like a group
/// cache. A real (non-x-ray) blur needs its own pair: a shared pair would make
/// every placement sample the last capture from the same texture.
struct SceneBlurCache {
    std::optional<OffscreenTarget> backdrop;
    std::optional<BlurChain> chain;
    int width = 0, height = 0;
};

/// What is currently on this output's KMS cursor plane. `source` is the identity
/// of the pixels up there — a `CursorImage*` for a themed cursor, a
/// `current_buffer_identity()` for a client one — so a moving pointer with an
/// unchanged image costs one `move_cursor` and no upload. `surface` is the
/// client sprite whose frame callbacks the plane owes, since no frame draws it.
struct SceneCursorPlane {
    const void* source = nullptr;
    SurfaceId surface;
    int x = 0;
    int y = 0;
    bool shown = false;
};

/// A surface the CPU path drew, and the commit that has to wake its output.
/// The GPU path's `Frame` keeps the same watch on everything it places.
struct SceneCommitWatch {
    SurfaceId surface;
    Signal<SurfaceCommit>::Connection committed;
};

struct SceneOutput::Impl {
    std::optional<Frame> frame;
    CpuCompositor cpu;
    SceneCursorPlane cursor_plane;
    // Reused every frame; cleared, never freed. `drawn` is the batch
    // frame-callback list the CPU path has to keep for itself, having no
    // `Frame` to remember placements, and `tree` is the per-tree walk scratch.
    std::vector<CpuItem> items;
    std::vector<SurfaceId> drawn;
    std::vector<SurfaceAt> tree;
    std::vector<SceneCommitWatch> watched;
    // What the last CPU frame that reached the screen was made of, so that an
    // identical one is not composited and committed again. `serials` runs
    // parallel to `items` (the caller's image serial, the cursor image's
    // identity); the `last_*` vectors are swapped with the live ones after a
    // composite rather than copied, so a steady frame allocates nothing.
    std::vector<std::uint64_t> serials;
    std::vector<CpuItem> last_items;
    std::vector<std::uint64_t> last_serials;
    std::vector<SurfaceId> last_drawn;
    Color last_background;
    int last_width = 0, last_height = 0;
    // `cpu_valid`: the pixels on screen came from the `last_*` description.
    // `cpu_dirty`: a drawn surface has committed since — a new buffer changes
    // nothing in the description, so this is what notices it.
    bool cpu_valid = false;
    bool cpu_dirty = false;
    // Which path the frame on screen came from: a GPU output that fell back
    // drew `drawn`, not its placement list.
    bool drew_on_cpu = false;
    // Per output rather than per renderer, so two monitors do not evict each
    // other's bar every frame. Indexed by the image's position among the
    // frame's image items, which is stable for a scene built the same way twice.
    std::vector<SceneImageCache> images;
    // Allocated the first time a scene actually asks for x-ray blur, and
    // dropped with the rest of the output's targets by reset(). A compositor
    // that never blurs never pays for either — which is what makes blur-off
    // free rather than merely cheap. X-ray is deliberately one shared cache:
    // one backdrop below the lowest x-ray item feeds the whole stack.
    SceneBlurCache xray;
    std::vector<SceneGroupCache> groups;
    // One per non-x-ray blurred item, indexed like the group caches.
    std::vector<SceneBlurCache> blurs;

    // The binding reset() makes. `owner` lists this state among its outputs;
    // both pointers are null while unbound.
    SceneRenderer::Impl* owner = nullptr;
    Output* output = nullptr;
    Signal<PresentEvent>::Connection on_present;
    Signal<OutputDestroy>::Connection on_destroy;
    Signal<OutputModeChange>::Connection on_mode;
    // The format the last reset() allocated in; empty until there was one, so a
    // state only ever presented on the CPU is not given GPU targets by a mode
    // change it never asked to follow.
    std::optional<std::uint32_t> format;
    // `present(..., layout)`'s scene moved into this output's coordinates.
    std::vector<SceneItem> local;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() { unbind(); }

    void bind(SceneRenderer::Impl& renderer, Output& out);
    void unbind() noexcept;
    void presented(const PresentEvent& event);
    void send_frame_done(std::uint32_t time_ms);
};

struct SceneRenderer::Impl {
    SceneStats stats;
    std::unique_ptr<VulkanRenderer> gpu;
    std::string gpu_error;
    // The themed cursor, in both currencies. Two keys and not one: an output
    // that fell back to the CPU must not invalidate the texture the GPU output
    // next to it is still placing.
    std::optional<GpuTexture> cursor_texture;
    const CursorImage* cursor_texture_of = nullptr;
    std::vector<Pixel> cursor_pixels;
    const CursorImage* cursor_pixels_of = nullptr;
    SceneCursorSource cursor;
    Presentation* presentation = nullptr;
    // Every bound output, so the pointer can be moved on all of them at once.
    std::vector<SceneOutput::Impl*> outputs;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        // States are meant to die first; one that did not must not unregister
        // itself from a renderer that is already gone.
        for (SceneOutput::Impl* state : outputs) {
            state->owner = nullptr;
        }
    }

    [[nodiscard]] SceneCursor cursor_on(const Output& output) const {
        return cursor ? cursor(output) : SceneCursor{};
    }
};

namespace {

/// The border as four solid rectangles just inside `box`. Top and bottom span
/// the full width and the sides fill only what is between them, so no pixel is
/// blended twice — which would show the moment a border is translucent.
template <class Emit>
void border_rects(const Box& box, int thickness, Emit&& emit) {
    if (thickness <= 0 || box.empty()) {
        return;
    }
    const int x = std::min(thickness, box.width / 2);
    const int y = std::min(thickness, box.height / 2);
    emit(box.x, box.y, box.width, y);
    emit(box.x, box.y + box.height - y, box.width, y);
    emit(box.x, box.y + y, x, box.height - 2 * y);
    emit(box.x + box.width - x, box.y + y, x, box.height - 2 * y);
}

[[nodiscard]] bool wants_blur(const SceneItem& item) noexcept {
    return item.blur.enabled && !item.box.empty();
}

void place_surface_item(Frame& frame, Surface& root, const SceneItem& item) {
    const int root_w = root.surface_width() > 0 ? root.surface_width() : item.box.width;
    const int root_h = root.surface_height() > 0 ? root.surface_height() : item.box.height;
    const Box surface{item.x, item.y, root_w, root_h};
    const Box visible = surface.intersection(item.box);
    if (surface.empty() || visible.empty()) {
        return;
    }
    // The crop clips the whole tree to the item's box, so a window that has not
    // yet answered a resize never paints over its neighbour.
    const auto transform =
        PlacementTransform::at(static_cast<float>(visible.x), static_cast<float>(visible.y),
                               static_cast<float>(visible.width),
                               static_cast<float>(visible.height))
            .crop(static_cast<float>(visible.x - surface.x) / surface.width,
                  static_cast<float>(visible.y - surface.y) / surface.height,
                  static_cast<float>(visible.width) / surface.width,
                  static_cast<float>(visible.height) / surface.height);
    frame.place(root, transform);
}

/// Where a composed window goes back, and what is done to it on the way: its
/// own opacity and its corner mask, applied to the one finished image.
[[nodiscard]] PlacementTransform effect_transform(const SceneItem& item) {
    return PlacementTransform::at(static_cast<float>(item.box.x), static_cast<float>(item.box.y),
                                  static_cast<float>(item.box.width),
                                  static_cast<float>(item.box.height))
        .opacity(std::clamp(item.alpha, 0.0f, 1.0f))
        .rounded(item.corner_radius);
}

/// A solid item's colour with the item's own opacity folded in — a rectangle
/// has nothing to compose offscreen, so its alpha is just less alpha.
[[nodiscard]] Color faded(const SceneItem& item) noexcept {
    Color color = item.color;
    color.a *= std::clamp(item.alpha, 0.0f, 1.0f);
    return color;
}

/// True when this item cannot simply be placed: an opacity or a corner mask
/// has to be applied to the finished window, not to each of its surfaces.
[[nodiscard]] bool wants_group(const SceneItem& item) noexcept {
    return item.kind == SceneItem::Kind::surface &&
           (item.alpha < 1.0f || item.corner_radius > 0.0f);
}

/// Allocate this output's blur targets on first use. Answers false when there
/// is no memory for them, which costs the blur and nothing else.
[[nodiscard]] bool ensure_blur(SceneBlurCache& cache, VulkanRenderer& gpu, int width,
                               int height) {
    if (cache.chain.has_value() && cache.chain->width() == width &&
        cache.chain->height() == height) {
        return true;
    }
    cache.backdrop.reset();
    cache.chain.reset();
    Result<OffscreenTarget> backdrop = gpu.create_offscreen(width, height);
    if (!backdrop) {
        return false;
    }
    Result<BlurChain> chain = gpu.create_blur_chain(width, height);
    if (!chain) {
        return false;
    }
    cache.backdrop.emplace(std::move(*backdrop));
    cache.chain.emplace(std::move(*chain));
    return true;
}

void place_image_item(Frame& frame, VulkanRenderer& gpu, SceneImageCache& cache,
                      const SceneItem& item) {
    const auto need = static_cast<std::size_t>(item.box.width) * item.box.height;
    // The caller's word about its own buffer is checked here too: a short span
    // is a bug, but it must not become a read past the end of one.
    if (item.box.empty() || item.box.width <= 0 || item.pixels.size() < need) {
        return;
    }
    if (!cache.texture.has_value() || cache.serial != item.serial) {
        Result<GpuTexture> texture = gpu.upload_texture(
            item.box.width, item.box.height,
            {reinterpret_cast<const std::uint8_t*>(item.pixels.data()), need * sizeof(Pixel)});
        if (!texture) {
            return;
        }
        cache.texture.emplace(std::move(*texture));
        cache.serial = item.serial;
    }
    // Through the transform rather than the four-int overload: an image is a
    // single texture, which is the one placement `rounded()` applies to, and
    // the bar is the caller who wants both. The serial goes with it: the cache
    // re-uploads in place, so the texture's address alone would tell the damage
    // diff that a repainted bar is the picture already on screen.
    frame.place(*cache.texture, effect_transform(item), item.serial);
}

void push_item(SceneOutput::Impl& state, CpuItem item, std::uint64_t serial = 0) {
    state.items.push_back(item);
    state.serials.push_back(serial);
}

/// Append one surface tree to the CPU draw list, and record every surface of it
/// — roots and subsurfaces — in `drawn`, which is where the CPU path's batch
/// frame-callback release comes from.
void append_tree(SceneOutput::Impl& state, const Output& output, Surface& root, int logical_x,
                 int logical_y, const Box& clip, float alpha = 1.0f) {
    state.tree.clear();
    root.surface_tree(state.tree);
    for (const SurfaceAt& at : state.tree) {
        state.drawn.push_back(at.surface->id());
    }
    const Box origin = output.to_device({logical_x, logical_y, 1, 1});
    push_item(state, CpuView{root.id(), origin.x, origin.y, clip, alpha});
}

/// Whether two CPU items draw the same pixels. Images compare by where their
/// pixels live and by `serial`, as the GPU path's texture cache does; the
/// contents are the caller's word.
[[nodiscard]] bool same_item(const CpuItem& a, const CpuItem& b) noexcept {
    if (a.index() != b.index()) {
        return false;
    }
    switch (a.index()) {
    case 0: {
        const auto& x = std::get<RectFill>(a);
        const auto& y = std::get<RectFill>(b);
        return x.box == y.box && x.color == y.color;
    }
    case 1: {
        const auto& x = std::get<CpuView>(a);
        const auto& y = std::get<CpuView>(b);
        return x.surface == y.surface && x.x == y.x && x.y == y.y && x.clip == y.clip &&
               x.alpha == y.alpha && x.scale == y.scale;
    }
    default: {
        const auto& x = std::get<CpuImage>(a);
        const auto& y = std::get<CpuImage>(b);
        return x.box == y.box && x.alpha == y.alpha && x.pixels.data() == y.pixels.data() &&
               x.pixels.size() == y.pixels.size();
    }
    }
}

/// Milliseconds on the clock `PresentEvent` reports, for the frame callbacks
/// released when nothing is presented.
[[nodiscard]] std::uint32_t monotonic_ms() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

/// Put `cursor` on the output's KMS cursor plane, if it has one and can take
/// this image. Returns true when hardware owns the pointer — including when
/// there is no pointer to show and the plane was what had been showing it.
[[nodiscard]] bool cursor_plane(SceneOutput::Impl& state, Output& output,
                                const SceneCursor& cursor) {
    if (!output.has_cursor_plane()) {
        return false;
    }
    SceneCursorPlane& plane = state.cursor_plane;

    const auto take_down = [&] {
        if (plane.shown) {
            std::ignore = output.hide_cursor();
        }
        plane = {};
    };

    // No pointer to draw. Answering true (handled) is only right if the plane
    // is what was showing it: if the last frame composited the cursor instead —
    // because the plane refused that image — the caller still owes a repaint to
    // get it off the screen.
    Surface* client = surface_from_id(cursor.surface);
    if (cursor.image == nullptr && client == nullptr) {
        const bool was_on_plane = plane.shown;
        take_down();
        return was_on_plane;
    }

    // The pixels, resolved only when the source actually changed. A pointer
    // sliding across the screen keeps the same image, which is the whole reason
    // the plane is cheap.
    const void* source = client != nullptr
                             ? client->current_buffer_identity()
                             : static_cast<const void*>(cursor.image);
    if (source == nullptr) {
        take_down();
        return true; // a cursor surface with no buffer is a hidden cursor
    }
    if (source != plane.source) {
        std::vector<std::uint8_t> rgba;
        int width = 0;
        int height = 0;
        if (client != nullptr) {
            if (!client->current_buffer_rgba(rgba, width, height)) {
                take_down();
                return false;
            }
        } else {
            width = cursor.image->width;
            height = cursor.image->height;
            rgba.assign(cursor.image->rgba.begin(), cursor.image->rgba.end());
        }
        // The hotspot is already off: `SceneCursor::x/y` is the image's
        // top-left, so the plane is told a zero hotspot and the same point.
        if (!output.set_cursor(rgba, width, height, 0, 0)) {
            // Too large for the plane, or a format it will not take. Composited
            // from here on — and taken down first, or the stale image stays up
            // underneath the one being drawn.
            take_down();
            return false;
        }
        plane.source = source;
        plane.surface = client != nullptr ? client->id() : SurfaceId{};
        plane.shown = false; // position not applied to this image yet
        // The new image is on the display now; no frame drew it, so no
        // `present` will release an animated sprite's next frame either.
        if (client != nullptr) {
            client->send_frame_done(monotonic_ms());
        }
    }

    if (!plane.shown || plane.x != cursor.x || plane.y != cursor.y) {
        if (!output.move_cursor(cursor.x, cursor.y)) {
            take_down();
            return false;
        }
        plane.x = cursor.x;
        plane.y = cursor.y;
        plane.shown = true;
    }
    return true;
}

} // namespace

namespace {
Status reallocate(SceneRenderer::Impl& renderer, SceneOutput::Impl& self, Output& output,
                  std::uint32_t drm_format);
} // namespace

void SceneOutput::Impl::bind(SceneRenderer::Impl& renderer, Output& out) {
    if (owner == &renderer && output == &out) {
        return;
    }
    unbind();
    owner = &renderer;
    output = &out;
    renderer.outputs.push_back(this);
    on_present = out.present.connect([this](PresentEvent& event) { presented(event); });
    // Everything here that holds the output goes while it still exists.
    on_destroy = out.destroy.connect([this](OutputDestroy&) { unbind(); });
    // Everything sized for the old mode is stale. Connected before anything
    // the compositor hooks up on this output, so it runs first.
    on_mode = out.mode_changed.connect([this](OutputModeChange&) {
        if (owner != nullptr && output != nullptr && format.has_value()) {
            (void)reallocate(*owner, *this, *output, *format);
        }
    });
}

void SceneOutput::Impl::unbind() noexcept {
    if (owner != nullptr) {
        std::erase(owner->outputs, this);
    }
    frame.reset();
    watched.clear();
    cpu_valid = false;
    cursor_plane = {};
    owner = nullptr;
    output = nullptr;
    on_present.disconnect();
    on_destroy.disconnect();
    on_mode.disconnect();
    format.reset();
}

void SceneOutput::Impl::send_frame_done(std::uint32_t time_ms) {
    if (frame.has_value() && !drew_on_cpu) {
        frame->send_frame_done(time_ms);
    } else {
        luminaria::send_frame_done(drawn, time_ms);
    }
    if (Surface* sprite = surface_from_id(cursor_plane.surface); sprite != nullptr) {
        sprite->send_frame_done(time_ms);
    }
}

void SceneOutput::Impl::presented(const PresentEvent& event) {
    // First: it is what lets a directly scanned-out client have its buffer back.
    if (frame.has_value()) {
        frame->presented();
    }
    // One stamp for the whole frame: every client that drew it paces to the
    // same presentation.
    send_frame_done(event.time_ms());
    Presentation* feedback = owner != nullptr ? owner->presentation : nullptr;
    if (feedback == nullptr) {
        return;
    }
    const auto notify = [&](SurfaceId id) {
        if (Surface* surface = surface_from_id(id); surface != nullptr) {
            feedback->notify_presented(*surface, event);
        }
    };
    if (frame.has_value() && !drew_on_cpu) {
        for (const Placement& placement : frame->placements()) {
            notify(placement.surface);
        }
    } else {
        for (const SurfaceId id : drawn) {
            notify(id);
        }
    }
}

SceneOutput::SceneOutput() : impl_(std::make_unique<Impl>()) {}
SceneOutput::~SceneOutput() = default;
SceneOutput::SceneOutput(SceneOutput&&) noexcept = default;
SceneOutput& SceneOutput::operator=(SceneOutput&&) noexcept = default;

bool SceneOutput::gpu_output() const noexcept { return impl_->frame.has_value(); }

SceneRenderer::SceneRenderer() : SceneRenderer(VulkanOptions{}) {}

SceneRenderer::SceneRenderer(const VulkanOptions& options) : impl_(std::make_unique<Impl>()) {
    Result<VulkanRenderer> gpu = VulkanRenderer::create(options);
    if (gpu) {
        impl_->gpu = std::make_unique<VulkanRenderer>(std::move(*gpu));
    } else {
        impl_->gpu_error = gpu.error().message;
    }
}

SceneRenderer::~SceneRenderer() = default;
SceneRenderer::SceneRenderer(SceneRenderer&&) noexcept = default;
SceneRenderer& SceneRenderer::operator=(SceneRenderer&&) noexcept = default;

bool SceneRenderer::gpu_enabled() const noexcept { return impl_->gpu != nullptr; }

VulkanRenderer* SceneRenderer::gpu() noexcept { return impl_->gpu.get(); }

std::string_view SceneRenderer::gpu_error() const noexcept { return impl_->gpu_error; }

void SceneRenderer::set_presentation(Presentation* presentation) noexcept {
    impl_->presentation = presentation;
}

void SceneRenderer::set_cursor_source(SceneCursorSource source) {
    impl_->cursor = std::move(source);
}

namespace {
Status reallocate(SceneRenderer::Impl& renderer, SceneOutput::Impl& self, Output& output,
                  std::uint32_t drm_format) {
    self.format = drm_format;
    // Dropped first: every texture it cached describes the old mode, and so do
    // the scanout buffers it would otherwise keep alive while allocating more.
    self.frame.reset();
    self.images.clear();
    self.cpu_valid = false; // the new mode's buffer is blank
    if (renderer.gpu == nullptr) {
        return ok();
    }
    self.frame.emplace(output, *renderer.gpu);
    if (Status ready = self.frame->reset(drm_format); !ready) {
        self.frame.reset();
        return ready;
    }
    return ok();
}
} // namespace

Status SceneRenderer::reset(SceneOutput& state, Output& output, std::uint32_t drm_format) {
    SceneOutput::Impl& self = *state.impl_;
    self.bind(*impl_, output);
    return reallocate(*impl_, self, output, drm_format);
}

Result<SceneOutcome> SceneRenderer::present(SceneOutput& state, Output& output,
                                            std::span<const SceneItem> scene, Color background,
                                            const OutputLayout& layout) {
    const Box view = layout.box_of(output);
    if (view.x == 0 && view.y == 0) {
        return present(state, output, scene, background);
    }
    // Into the state's own scratch: cleared, never freed, so a steady frame
    // still allocates nothing.
    std::vector<SceneItem>& local = state.impl_->local;
    local.assign(scene.begin(), scene.end());
    for (SceneItem& item : local) {
        item.box.x -= view.x;
        item.box.y -= view.y;
        item.x -= view.x;
        item.y -= view.y;
        if (!item.shadow_box.empty()) {
            item.shadow_box.x -= view.x;
            item.shadow_box.y -= view.y;
        }
    }
    return present(state, output, local, background);
}

void SceneRenderer::update_cursor() {
    // By index: nothing below binds or unbinds, but nothing here should have to
    // know that.
    for (std::size_t i = 0; i < impl_->outputs.size(); ++i) {
        SceneOutput::Impl& state = *impl_->outputs[i];
        Output& output = *state.output;
        if (!cursor_plane(state, output, impl_->cursor_on(output))) {
            output.schedule_frame();
        }
    }
}

SceneStats SceneRenderer::stats() const noexcept { return impl_->stats; }

Result<SceneOutcome> SceneRenderer::present(SceneOutput& state, Output& output,
                                            std::span<const SceneItem> scene, Color background) {
    Result<SceneOutcome> outcome = draw(state, output, scene, background);
    if (outcome) {
        SceneStats& stats = impl_->stats;
        switch (*outcome) {
        case SceneOutcome::composited: ++stats.composited; break;
        case SceneOutcome::scanout: ++stats.scanout; break;
        case SceneOutcome::unchanged: ++stats.unchanged; break;
        case SceneOutcome::cpu: ++stats.cpu; break;
        }
    }
    return outcome;
}

Result<SceneOutcome> SceneRenderer::draw(SceneOutput& state, Output& output,
                                         std::span<const SceneItem> scene, Color background) {
    SceneOutput::Impl& self = *state.impl_;
    // A compositor with no GPU has nothing to reset(), but its frames still
    // have to be answered.
    self.bind(*impl_, output);
    const SceneCursor cursor = impl_->cursor_on(output);
    // Hardware first: if the plane takes the pointer, it is already on screen
    // and must not also be composited into the frame.
    const bool cursor_on_plane = cursor_plane(self, output, cursor);

    if (self.frame.has_value()) {
        Frame& frame = *self.frame;
        frame.begin({0, 0, output.logical_width(), output.logical_height()});

        const Box view{0, 0, output.logical_width(), output.logical_height()};
        const int scale = std::max(1, output.scale());
        // Everything is a potential blur backdrop, so the marker is the bottom
        // of the frame. A blurred item captures what has been placed under it
        // by the time its turn comes.
        const PlacementGroup bottom = frame.begin_group();
        bool shared_backdrop = false;

        // The caches below are indexed by position and grow on demand, but the
        // frame borrows into them across the whole loop: a queued blur job
        // holds the OffscreenTarget/BlurChain it will capture into at submit()
        // time, and a composed group or image placement holds the texture it
        // will draw. Growing a cache vector mid-loop would move those objects
        // and dangle every borrow taken before the reallocation, so reserve
        // once, up front — each index increments at most once per scene item,
        // which makes scene.size() a safe upper bound for all three.
        self.blurs.reserve(scene.size());
        self.groups.reserve(scene.size());
        self.images.reserve(scene.size());

        std::size_t image = 0;
        std::size_t group = 0;
        std::size_t blur = 0; // non-x-ray blurred items so far
        for (const SceneItem& item : scene) {
            if (item.shadow.color.a > 0.0f && item.shadow.feather > 0.0f) {
                const Box caster = item.shadow_box.empty() ? item.box : item.shadow_box;
                frame.place_shadow(caster, item.shadow.color, item.corner_radius,
                                   item.shadow.feather);
            }
            if (wants_blur(item)) {
                const int spread = blur_spread(item.blur.params);
                // `surface_regions` blurs only what the client declared: an
                // item that declared nothing places nothing, and an empty
                // request queues no capture at all. A non-surface item has no
                // tree to ask, so it takes the whole-box arm like a compositor
                // that never enabled the client hints.
                const bool regions =
                    item.blur.surface_regions && item.kind == SceneItem::Kind::surface;
                // A real (non-x-ray) blur captures into its own backdrop and
                // chain, indexed by its position among blurred items like a
                // group cache — a shared pair would make every placement sample
                // the last capture from the same texture. X-ray is the one
                // deliberately shared cache: one backdrop below the lowest
                // x-ray item feeds the whole stack, and that IS x-ray, because
                // none of them ever sees the window below it.
                SceneBlurCache* cache = nullptr;
                if (item.blur.xray) {
                    cache = &self.xray;
                } else {
                    if (self.blurs.size() <= blur) {
                        self.blurs.resize(blur + 1);
                    }
                    cache = &self.blurs[blur++];
                }
                const bool have_targets =
                    ensure_blur(*cache, *impl_->gpu, view.width * scale, view.height * scale);
                if (item.blur.xray && shared_backdrop) {
                    // The one shared capture is already queued below the lowest
                    // x-ray item; place only, sampling the same chain texture.
                    if (regions) {
                        if (Surface* root = surface_from_id(item.surface); root != nullptr) {
                            (void)frame.place_blur_regions(self.xray.chain->texture(), view,
                                                           *root, item.x, item.y, item.box,
                                                           spread);
                        }
                    } else {
                        frame.place_blur(self.xray.chain->texture(), view, item.box,
                                         item.corner_radius, spread);
                    }
                } else if (regions && have_targets) {
                    if (Surface* root = surface_from_id(item.surface); root != nullptr &&
                        frame.queue_blur_regions(bottom, *cache->backdrop, *cache->chain, view,
                                                 item.blur.params, *root, item.x, item.y,
                                                 item.box, spread)) {
                        shared_backdrop = shared_backdrop || item.blur.xray;
                    }
                } else if (have_targets) {
                    if (frame.queue_blur(bottom, *cache->backdrop, *cache->chain, view,
                                         item.blur.params, item.box, item.corner_radius,
                                         spread)) {
                        shared_backdrop = shared_backdrop || item.blur.xray;
                    }
                }
            }
            const PlacementGroup window = frame.begin_group();
            switch (item.kind) {
            case SceneItem::Kind::rect:
                frame.place_rect(item.box.x, item.box.y, item.box.width, item.box.height,
                                 faded(item));
                break;
            case SceneItem::Kind::border:
                border_rects(item.box, item.thickness, [&](int x, int y, int w, int h) {
                    frame.place_rect(x, y, w, h, faded(item));
                });
                break;
            case SceneItem::Kind::image:
                if (self.images.size() <= image) {
                    self.images.resize(image + 1);
                }
                place_image_item(frame, *impl_->gpu, self.images[image++], item);
                break;
            case SceneItem::Kind::surface:
                if (Surface* root = surface_from_id(item.surface); root != nullptr) {
                    place_surface_item(frame, *root, item);
                }
                break;
            }
            if (!wants_group(item)) {
                continue;
            }
            // Fade or round the finished window rather than each of its
            // surfaces, or the overlaps blend twice and seam (ADR 0005). A
            // target that cannot be allocated costs the effect, not the window.
            if (self.groups.size() <= group) {
                self.groups.resize(group + 1);
            }
            SceneGroupCache& cache = self.groups[group++];
            const int w = item.box.width * scale;
            const int h = item.box.height * scale;
            if (!cache.target.has_value() || cache.width != w || cache.height != h) {
                cache.target.reset();
                if (Result<OffscreenTarget> target = impl_->gpu->create_offscreen(w, h); target) {
                    cache.target.emplace(std::move(*target));
                    cache.width = w;
                    cache.height = h;
                }
            }
            if (cache.target.has_value()) {
                (void)frame.compose_group(window, *cache.target, item.box,
                                          effect_transform(item));
            }
        }

        if (cursor_on_plane) {
            // Nothing to place: the KMS plane is showing it.
        } else if (Surface* root = surface_from_id(cursor.surface); root != nullptr) {
            frame.place(*root, cursor.x, cursor.y);
        } else if (cursor.image != nullptr) {
            if (!impl_->cursor_texture.has_value() || impl_->cursor_texture_of != cursor.image) {
                if (Result<GpuTexture> texture = impl_->gpu->upload_texture(
                        cursor.image->width, cursor.image->height, cursor.image->rgba);
                    texture) {
                    impl_->cursor_texture.emplace(std::move(*texture));
                    impl_->cursor_texture_of = cursor.image;
                }
            }
            if (impl_->cursor_texture.has_value()) {
                frame.place(*impl_->cursor_texture, cursor.x, cursor.y, cursor.image->width,
                            cursor.image->height);
            }
        }

        Result<Presented> shown = frame.submit(background);
        if (shown && *shown != Presented::fallback) {
            if (*shown == Presented::unchanged) {
                // Nothing was committed, so no `present` follows: release the
                // callbacks now, or a client that committed without damaging
                // anything waits for good. There is no feedback to give.
                self.send_frame_done(monotonic_ms());
                return SceneOutcome::unchanged;
            }
            self.drew_on_cpu = false;
            self.cpu_valid = false; // what the CPU last drew is not on screen now
            // Nested and headless outputs have no page flip to report this
            // commit; one paced event delivers presentation and then stops.
            output.schedule_frame();
            return *shown == Presented::scanout ? SceneOutcome::scanout
                                                : SceneOutcome::composited;
        }
        // `fallback`: the GPU could not composite at all. That is what the rest
        // of this function is for.
    }

    const int width = output.width();
    const int height = output.height();
    if (width <= 0 || height <= 0) {
        return fail("output has no framebuffer to draw into");
    }

    self.items.clear();
    self.serials.clear();
    self.drawn.clear();
    for (const SceneItem& item : scene) {
        const Box device = output.to_device(item.box);
        switch (item.kind) {
        case SceneItem::Kind::rect:
            if (!device.empty()) {
                push_item(self, RectFill{device, faded(item)});
            }
            break;
        case SceneItem::Kind::border:
            border_rects(device, item.thickness * output.scale(), [&](int x, int y, int w, int h) {
                if (w > 0 && h > 0) {
                    push_item(self, RectFill{{x, y, w, h}, faded(item)});
                }
            });
            break;
        case SceneItem::Kind::image:
            // The image is rasterized at its logical size, so a scaled output
            // would want it rasterized larger rather than stretched here.
            if (!device.empty()) {
                push_item(self, CpuImage{device, item.pixels, item.alpha}, item.serial);
            }
            break;
        case SceneItem::Kind::surface:
            if (Surface* root = surface_from_id(item.surface); root != nullptr) {
                // No blur, no shadow and square corners: this path exists so a
                // machine with no GPU still shows a desktop, and refusing to
                // draw a window because it asked to be rounded would be the
                // opposite of that. Opacity does work.
                append_tree(self, output, *root, item.x, item.y, device, item.alpha);
            }
            break;
        }
    }

    if (cursor_on_plane) {
        // Already on the KMS plane; compositing it here would double it.
    } else if (Surface* root = surface_from_id(cursor.surface); root != nullptr) {
        // The cursor is confined by nothing but the framebuffer: an empty clip
        // means the whole output.
        append_tree(self, output, *root, cursor.x, cursor.y, {});
    } else if (cursor.image != nullptr) {
        // Widened to `Pixel` once per cursor rather than reinterpreted per
        // frame: the source is a byte vector, and a cast between the two is the
        // kind of aliasing this library does not do to save 2 KiB of copy.
        if (impl_->cursor_pixels_of != cursor.image) {
            const std::span<const std::uint8_t> rgba{cursor.image->rgba};
            impl_->cursor_pixels.assign(rgba.size() / 4, Pixel{});
            for (std::size_t i = 0; i < impl_->cursor_pixels.size(); ++i) {
                impl_->cursor_pixels[i] = {rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2],
                                           rgba[i * 4 + 3]};
            }
            impl_->cursor_pixels_of = cursor.image;
        }
        const Box place =
            output.to_device({cursor.x, cursor.y, cursor.image->width, cursor.image->height});
        push_item(self,
                  CpuImage{{place.x, place.y, cursor.image->width, cursor.image->height},
                           impl_->cursor_pixels},
                  reinterpret_cast<std::uintptr_t>(cursor.image));
    }

    // The CPU path's `unchanged`: the same description of the same screen, and
    // nothing drawn has committed since. A client committing a new buffer
    // changes none of the keys, which is what `cpu_dirty` is for. Skipping
    // means no commit, so no `present` and no further frame is asked for: the
    // callbacks are released here and the output goes quiet until a commit or
    // the compositor wakes it.
    if (self.cpu_valid && !self.cpu_dirty && self.last_width == width &&
        self.last_height == height && self.last_background == background &&
        self.drawn == self.last_drawn && self.serials == self.last_serials &&
        std::ranges::equal(self.items, self.last_items, same_item)) {
        self.send_frame_done(monotonic_ms());
        return SceneOutcome::unchanged;
    }

    self.cpu_valid = false;
    self.cpu.composite(width, height, background, self.items);
    if (Status committed = output.commit_frame(self.cpu.pixels(), width, height); !committed) {
        return std::unexpected(committed.error());
    }
    self.drew_on_cpu = true;
    self.cpu_valid = true;
    self.cpu_dirty = false;
    self.last_width = width;
    self.last_height = height;
    self.last_background = background;
    self.last_drawn = self.drawn; // capacity kept: no allocation once steady
    std::swap(self.last_items, self.items);
    std::swap(self.last_serials, self.serials);
    // No `Frame` is watching these surfaces, so their next commit would never
    // reach the screen. One connection per surface for as long as it is drawn;
    // a steady scene adds none.
    std::erase_if(self.watched, [](const SceneCommitWatch& watch) {
        return surface_from_id(watch.surface) == nullptr;
    });
    for (const SurfaceId id : self.drawn) {
        Surface* surface = surface_from_id(id);
        if (surface == nullptr ||
            std::ranges::find(self.watched, id, &SceneCommitWatch::surface) !=
                self.watched.end()) {
            continue;
        }
        Output* out = &output;
        SceneOutput::Impl* state = &self;
        self.watched.push_back({id, surface->commit.connect([out, state](SurfaceCommit&) {
                                    state->cpu_dirty = true;
                                    out->schedule_frame();
                                })});
    }
    // Software outputs report presentation immediately before the next frame;
    // ask for that one event so frame callbacks are paced to what was shown.
    output.schedule_frame();
    return SceneOutcome::cpu;
}

Result<std::span<const Pixel>> SceneRenderer::read_back(SceneOutput& state) {
    if (state.impl_->frame.has_value()) {
        return state.impl_->frame->read_back();
    }
    return state.impl_->cpu.pixels();
}

} // namespace luminaria
