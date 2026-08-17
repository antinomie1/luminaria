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
// A compositor that wants the GPU path only can still drive `Frame` directly;
// this is for the ones that must also come up on a machine with no GPU — which
// includes every test suite that runs in CI.
export module luminaria.gpu:scene_renderer;

import std;

import luminaria;

import :frame;
import :vulkan;

export namespace luminaria {

/// The cursor, drawn over everything else: either a client surface tree
/// (`wl_pointer.set_cursor`) or a compositor-owned image (a themed cursor on a
/// backend with no cursor plane). Visual only, and therefore never in the scene
/// list and never hit-tested.
struct SceneCursor {
    SurfaceId surface{};
    const CursorImage* image = nullptr;
    int x = 0, y = 0;
};

/// What `SceneRenderer::present()` did. `unchanged` means nothing was committed
/// and no `present` event will follow, so whatever the compositor does there —
/// `send_frame_done()` above all — it must do on this answer too.
enum class SceneOutcome { committed, unchanged };

/// Per-output renderer state: the GPU frame ledger when there is one, the CPU
/// compositor when there is not, and the scratch both reuse so that a
/// steady-state frame allocates nothing.
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

    /// Allocate everything sized for `output`'s CURRENT mode. Call it once per
    /// output and again from `Output::mode_changed`.
    ///
    /// A failure is not fatal and is not an error the caller has to handle
    /// beyond reporting it: this output falls back to the CPU path, which needs
    /// no setup. There is nothing to reset when there is no GPU at all, and
    /// that answers `ok()`.
    Status reset(SceneOutput& state, Output& output, std::uint32_t drm_format);

    /// Draw `scene` and present it on `output`.
    ///
    /// The scene is in the output's *logical* coordinates. On the GPU path
    /// `Frame` applies the output's scale and transform; the CPU path draws
    /// each surface tree at its logical size with the clip carried in device
    /// coordinates — byte-exact for scale=1 and a normal transform, which is
    /// what every software output currently is.
    [[nodiscard]] Result<SceneOutcome> present(SceneOutput& state, Output& output,
                                               std::span<const SceneItem> scene, Color background,
                                               const SceneCursor& cursor = {});

    /// From the output's `present` handler.
    void presented(SceneOutput& state);

    /// Release every frame callback the last `present()` owed, at one stamp.
    /// Call it from `Output::present`, and again when `present()` answered
    /// `unchanged` — that frame never reaches the display, so no `present`
    /// follows it and a client that committed without damage would freeze.
    void send_frame_done(SceneOutput& state, std::uint32_t time_ms);

    struct Impl;

private:
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

struct SceneOutput::Impl {
    std::optional<Frame> frame;
    CpuCompositor cpu;
    // Reused every frame; cleared, never freed. `drawn` is the batch
    // frame-callback list the CPU path has to keep for itself, having no
    // `Frame` to remember placements, and `tree` is the per-tree walk scratch.
    std::vector<CpuItem> items;
    std::vector<SurfaceId> drawn;
    std::vector<SurfaceAt> tree;
    // Per output rather than per renderer, so two monitors do not evict each
    // other's bar every frame. Indexed by the image's position among the
    // frame's image items, which is stable for a scene built the same way twice.
    std::vector<SceneImageCache> images;
};

struct SceneRenderer::Impl {
    std::unique_ptr<VulkanRenderer> gpu;
    std::string gpu_error;
    // The themed cursor, in both currencies. Two keys and not one: an output
    // that fell back to the CPU must not invalidate the texture the GPU output
    // next to it is still placing.
    std::optional<GpuTexture> cursor_texture;
    const CursorImage* cursor_texture_of = nullptr;
    std::vector<Pixel> cursor_pixels;
    const CursorImage* cursor_pixels_of = nullptr;
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

void place_surface_item(Frame& frame, Surface& root, const SceneItem& item) {
    const Box surface{item.x, item.y, root.surface_width(), root.surface_height()};
    const Box visible = surface.intersection(item.box);
    if (surface.empty() || visible.empty()) {
        return;
    }
    // The crop clips the whole tree to the item's box, so a window that has not
    // yet answered a resize never paints over its neighbour.
    const auto transform =
        PlacementTransform::at(static_cast<float>(surface.x), static_cast<float>(surface.y),
                               static_cast<float>(surface.width),
                               static_cast<float>(surface.height))
            .crop(static_cast<float>(visible.x - surface.x) / surface.width,
                  static_cast<float>(visible.y - surface.y) / surface.height,
                  static_cast<float>(visible.width) / surface.width,
                  static_cast<float>(visible.height) / surface.height);
    frame.place(root, transform);
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
    frame.place(*cache.texture, item.box.x, item.box.y, item.box.width, item.box.height);
}

/// Append one surface tree to the CPU draw list, and record every surface of it
/// — roots and subsurfaces — in `drawn`, which is where the CPU path's batch
/// frame-callback release comes from.
void append_tree(SceneOutput::Impl& state, const Output& output, Surface& root, int logical_x,
                 int logical_y, const Box& clip) {
    state.tree.clear();
    root.surface_tree(state.tree);
    for (const SurfaceAt& at : state.tree) {
        state.drawn.push_back(at.surface->id());
    }
    const Box origin = output.to_device({logical_x, logical_y, 1, 1});
    state.items.emplace_back(CpuView{root.id(), origin.x, origin.y, clip});
}

} // namespace

SceneOutput::SceneOutput() : impl_(std::make_unique<Impl>()) {}
SceneOutput::~SceneOutput() = default;
SceneOutput::SceneOutput(SceneOutput&&) noexcept = default;
SceneOutput& SceneOutput::operator=(SceneOutput&&) noexcept = default;

bool SceneOutput::gpu_output() const noexcept { return impl_->frame.has_value(); }

SceneRenderer::SceneRenderer() : impl_(std::make_unique<Impl>()) {
    Result<VulkanRenderer> gpu = VulkanRenderer::create();
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

Status SceneRenderer::reset(SceneOutput& state, Output& output, std::uint32_t drm_format) {
    // Dropped first: every texture it cached describes the old mode, and so do
    // the scanout buffers it would otherwise keep alive while allocating more.
    state.impl_->frame.reset();
    state.impl_->images.clear();
    if (impl_->gpu == nullptr) {
        return ok();
    }
    state.impl_->frame.emplace(output, *impl_->gpu);
    if (Status ready = state.impl_->frame->reset(drm_format); !ready) {
        state.impl_->frame.reset();
        return ready;
    }
    return ok();
}

void SceneRenderer::presented(SceneOutput& state) {
    if (state.impl_->frame.has_value()) {
        state.impl_->frame->presented();
    }
}

void SceneRenderer::send_frame_done(SceneOutput& state, std::uint32_t time_ms) {
    if (state.impl_->frame.has_value()) {
        state.impl_->frame->send_frame_done(time_ms);
    } else {
        luminaria::send_frame_done(state.impl_->drawn, time_ms);
    }
}

Result<SceneOutcome> SceneRenderer::present(SceneOutput& state, Output& output,
                                            std::span<const SceneItem> scene, Color background,
                                            const SceneCursor& cursor) {
    SceneOutput::Impl& self = *state.impl_;
    if (self.frame.has_value()) {
        Frame& frame = *self.frame;
        frame.begin({0, 0, output.logical_width(), output.logical_height()});

        std::size_t image = 0;
        for (const SceneItem& item : scene) {
            switch (item.kind) {
            case SceneItem::Kind::rect:
                frame.place_rect(item.box.x, item.box.y, item.box.width, item.box.height,
                                 item.color);
                break;
            case SceneItem::Kind::border:
                border_rects(item.box, item.thickness, [&](int x, int y, int w, int h) {
                    frame.place_rect(x, y, w, h, item.color);
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
        }

        if (Surface* root = surface_from_id(cursor.surface); root != nullptr) {
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
                return SceneOutcome::unchanged;
            }
            // Nested and headless outputs have no page flip to report this
            // commit; one paced event delivers presentation and then stops.
            output.schedule_frame();
            return SceneOutcome::committed;
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
    self.drawn.clear();
    for (const SceneItem& item : scene) {
        const Box device = output.to_device(item.box);
        switch (item.kind) {
        case SceneItem::Kind::rect:
            if (!device.empty()) {
                self.items.emplace_back(RectFill{device, item.color});
            }
            break;
        case SceneItem::Kind::border:
            border_rects(device, item.thickness * output.scale(), [&](int x, int y, int w, int h) {
                if (w > 0 && h > 0) {
                    self.items.emplace_back(RectFill{{x, y, w, h}, item.color});
                }
            });
            break;
        case SceneItem::Kind::image:
            // The image is rasterized at its logical size, so a scaled output
            // would want it rasterized larger rather than stretched here.
            if (!device.empty()) {
                self.items.emplace_back(CpuImage{device, item.pixels});
            }
            break;
        case SceneItem::Kind::surface:
            if (Surface* root = surface_from_id(item.surface); root != nullptr) {
                append_tree(self, output, *root, item.x, item.y, device);
            }
            break;
        }
    }

    if (Surface* root = surface_from_id(cursor.surface); root != nullptr) {
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
        self.items.emplace_back(
            CpuImage{{place.x, place.y, cursor.image->width, cursor.image->height},
                     impl_->cursor_pixels});
    }

    self.cpu.composite(width, height, background, self.items);
    if (Status committed = output.commit_frame(self.cpu.pixels(), width, height); !committed) {
        return std::unexpected(committed.error());
    }
    // Software outputs report presentation immediately before the next frame;
    // ask for that one event so frame callbacks are paced to what was shown.
    output.schedule_frame();
    return SceneOutcome::committed;
}

} // namespace luminaria
