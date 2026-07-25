// luminaria/compositor.cppm — the wl_compositor global and the wl_surface objects it
// mints. This is where clients' windows enter the compositor.
//
// A wl_surface can also be a *subsurface* of another (wl_subcompositor, see
// luminaria/subcompositor.cppm): a child positioned relative to its parent and
// stacked below or above it. `surface_tree()` walks a surface and its
// subsurfaces back-to-front, which is what the renderer and hit-testing want.
//
// wl_resource stays opaque: it is forward-declared in the global module
// fragment, so importing luminaria pulls in no libwayland headers.

module;

#include "luminaria/detail/wayland_fwd.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

export module luminaria:compositor;

import :core.expected;
import :core.signal;
import :render.vulkan;
import :util.box;
import :util.region;
import :util.transform;

export namespace luminaria {

class Display;
class Surface;

struct NewSurface {
    Surface& surface;
};

struct SurfaceCommit {
    Surface& surface;
};

/// Fired just before a Surface is destroyed. Anything holding a `Surface*`
/// (seat focus, scene nodes, window lists) must drop it here.
struct SurfaceDestroy {
    Surface& surface;
};

/// "The GPU work that samples this surface's buffer has been submitted."
/// `fence_fd` is a sync_file that signals when that work is done, or -1 if it
/// already is. Borrowed for the duration of the emit — dup it to keep it.
/// linux-drm-syncobj turns this into the client's release point; without a
/// listener the buffer is released the ordinary way, on the next commit.
struct SurfaceRendered {
    Surface& surface;
    int fence_fd;
};

/// A surface inside a surface tree, with its offset relative to the tree root.
struct SurfaceAt {
    Surface* surface;
    int x;
    int y;
};

/// A client wl_surface. Owned by its wl_resource (lives until the client
/// destroys it); its address is stable, so signals may capture `Surface&`.
class Surface {
public:
    // Constructor and destructor are out-of-line: BufferWatch is incomplete here.
    explicit Surface(wl_resource* resource) noexcept;
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    /// Fires on each wl_surface.commit, after pending state becomes current.
    /// For a subsurface in effective sync mode this is deferred to the commit
    /// of its parent, as the protocol requires.
    Signal<SurfaceCommit> commit;
    /// Fires just before this Surface goes away. Drop every pointer to it here.
    Signal<SurfaceDestroy> destroy;
    /// Fires when the compositor has submitted the render that reads this
    /// surface (see SurfaceRendered).
    Signal<SurfaceRendered> rendered;

    /// True once a non-null buffer has been committed.
    [[nodiscard]] bool has_buffer() const noexcept { return current_buffer_ != nullptr; }

    /// Size of the committed buffer in pixels (0 if there is none).
    [[nodiscard]] int buffer_width() const noexcept { return buffer_width_; }
    [[nodiscard]] int buffer_height() const noexcept { return buffer_height_; }

    // --- surface coordinates ---
    //
    // A client may hand us a buffer that is denser than the surface
    // (set_buffer_scale, HiDPI) or stored rotated (set_buffer_transform), and
    // wp_viewporter lets it crop and stretch on top. Everything outside the
    // renderer — layout, hit-testing, subsurface offsets — works in SURFACE
    // coordinates, which is what these report. The buffer size above is only
    // for the code that touches pixels.

    /// Size of the surface in surface-local coordinates (0 if it has no buffer).
    [[nodiscard]] int surface_width() const noexcept;
    [[nodiscard]] int surface_height() const noexcept;

    /// The client's declared buffer scale (wl_surface.set_buffer_scale), >= 1.
    [[nodiscard]] int buffer_scale() const noexcept { return buffer_scale_; }
    /// How the buffer is stored relative to the surface — already inverted from
    /// wl_surface.set_buffer_transform, so it maps buffer -> surface and can be
    /// handed straight to the renderer.
    [[nodiscard]] Transform buffer_transform() const noexcept { return buffer_transform_; }

    /// Accumulated wl_surface.offset / attach(x,y): where this surface's content
    /// sits relative to where the compositor placed it.
    [[nodiscard]] int offset_x() const noexcept { return offset_x_; }
    [[nodiscard]] int offset_y() const noexcept { return offset_y_; }

    // --- regions ---

    /// Pixels the client promises are fully opaque, in surface coordinates.
    /// Empty means "assume nothing"; the renderer uses it to skip whatever is
    /// hidden behind this surface.
    [[nodiscard]] const Region& opaque_region() const noexcept { return opaque_; }
    /// Where this surface accepts pointer/touch input, in surface coordinates.
    /// Only meaningful when has_input_region() — the default is the whole
    /// surface, which no finite region can express.
    [[nodiscard]] const Region& input_region() const noexcept { return input_; }
    [[nodiscard]] bool has_input_region() const noexcept { return has_input_region_; }
    /// True if (sx,sy) in surface coordinates should reach this surface.
    [[nodiscard]] bool accepts_input(double sx, double sy) const noexcept;

    // --- viewport (wp_viewporter) ---

    /// Source crop in buffer coordinates, or an empty box for "the whole buffer".
    /// Fractional in the protocol; kept as a float box because a 1.5x scale
    /// really does land on half pixels.
    [[nodiscard]] bool has_viewport_source() const noexcept { return viewport_.has_source; }
    [[nodiscard]] double viewport_src_x() const noexcept { return viewport_.src_x; }
    [[nodiscard]] double viewport_src_y() const noexcept { return viewport_.src_y; }
    [[nodiscard]] double viewport_src_width() const noexcept { return viewport_.src_w; }
    [[nodiscard]] double viewport_src_height() const noexcept { return viewport_.src_h; }

    /// The part of the buffer this surface shows, as normalized BUFFER
    /// coordinates — exactly what `GpuTextureFill::u0..v1` wants. The whole
    /// buffer (0,0,1,1) unless a viewport crops it.
    void buffer_source_uv(float& u0, float& v0, float& u1, float& v1) const noexcept;

    // --- explicit sync (linux-drm-syncobj) ---

    /// A sync_file the GPU must wait on before sampling this surface's buffer,
    /// or -1. Borrowed: valid until the next commit, never close it. Feed it to
    /// VulkanRenderer::render_to so nothing blocks on the CPU.
    [[nodiscard]] int acquire_fence_fd() const noexcept { return acquire_fence_; }
    /// Called by the linux-drm-syncobj glue on commit; takes ownership of `fd`.
    void set_acquire_fence(int fd) noexcept;
    /// Called by the compositor once the render sampling this surface has been
    /// submitted. `fence_fd` is borrowed (-1 if the work is already complete).
    void notify_rendered(int fence_fd);

    /// Tell the client which buffer scale / transform would need no conversion
    /// on the output it is showing on (wl_compositor v6). Clients that listen
    /// render at the right density instead of guessing from wl_output.
    void set_preferred_buffer_scale(int scale);
    void set_preferred_buffer_transform(Transform transform);

    /// Escape hatch for protocol wiring (e.g. seat focus): the wl_surface resource.
    [[nodiscard]] wl_resource* c_resource() const noexcept { return resource_; }

    /// Copy the committed wl_shm buffer into tightly-packed RGBA8 (`out`), setting
    /// width/height. Returns false if there is no buffer or it isn't a supported
    /// shm format (ARGB8888/XRGB8888). This is the bridge into the renderer.
    [[nodiscard]] bool current_buffer_rgba(std::vector<std::uint8_t>& out, int& width,
                                           int& height) const;

    // --- damage ---
    //
    // Clients tell us which pixels changed; without that a compositor repaints
    // and re-scans-out the whole screen for a blinking cursor. Damage
    // accumulates across commits and is cleared by whoever consumed it.

    /// Regions changed since the last clear_damage(), in surface-local pixels.
    /// A newly attached buffer of a different size damages all of it.
    [[nodiscard]] const std::vector<Box>& damage() const noexcept { return damage_; }
    /// Damage was rendered; start accumulating again.
    void clear_damage() noexcept { damage_.clear(); }

    // --- frame callbacks ---

    /// Fire the wl_surface.frame callbacks queued since the last call, telling
    /// the client to draw again. Call this once the frame carrying this surface
    /// has actually been presented — pacing clients to the display is the whole
    /// point of the callback. `time_ms` is a CLOCK_MONOTONIC millisecond stamp.
    void send_frame_done(std::uint32_t time_ms);

    /// True if a client asked us to present this surface without waiting for
    /// vblank (wp_tearing_control_v1). Only meaningful for a fullscreen surface
    /// that owns the whole output.
    [[nodiscard]] bool tearing_hint() const noexcept { return tearing_; }
    /// Called by the wp_tearing_control_v1 glue; applied on the next commit.
    void set_pending_tearing_hint(bool async) noexcept { pending_.tearing = async; }

    /// The same bridge, but landing on the GPU: a dmabuf buffer is imported with
    /// no copy at all, shm and single-pixel buffers are uploaded once. This is
    /// what the compositing path wants — nothing here reads pixels back to the
    /// CPU.
    ///
    /// The result is CACHED on the surface. A dmabuf import is a live view of
    /// the client's pages, so it survives until the buffer is replaced; an shm
    /// upload is a copy, so it is redone on every commit. Either way the caller
    /// gets a borrowed pointer and stops paying to re-import an unchanged
    /// window every frame. Null if there is no buffer or it cannot be imported.
    ///
    /// LIFETIME: `renderer` must outlive this Surface — the cached texture
    /// belongs to it. In practice that means creating the VulkanRenderer before
    /// the Display and destroying it after.
    [[nodiscard]] GpuTexture* buffer_texture(VulkanRenderer& renderer);

    // --- subsurface tree ---

    /// This surface's parent, if it is a subsurface.
    [[nodiscard]] Surface* subsurface_parent() const noexcept { return parent_; }
    /// Position relative to the parent surface (0,0 if not a subsurface).
    [[nodiscard]] int subsurface_x() const noexcept { return sub_x_; }
    [[nodiscard]] int subsurface_y() const noexcept { return sub_y_; }

    /// This surface plus all its subsurfaces, back-to-front, with offsets
    /// relative to *this* surface. Always contains at least `{this, 0, 0}`.
    [[nodiscard]] std::vector<SurfaceAt> surface_tree();

    /// Topmost surface of this tree whose buffer covers (sx,sy), given in this
    /// surface's coordinates. The returned x/y is that surface's offset relative
    /// to this one — subtract it to get surface-local coordinates.
    [[nodiscard]] std::optional<SurfaceAt> surface_at(double sx, double sy);

    // --- internal: called by the protocol glue in compositor.cpp ---
    void set_pending_buffer(wl_resource* buffer);
    void add_pending_damage(int x, int y, int width, int height);
    /// wl_surface.damage_buffer: buffer coordinates, so it has to come back
    /// through the buffer scale and transform before it means anything.
    void add_pending_buffer_damage(int x, int y, int width, int height);
    void add_frame_callback(wl_resource* callback) { pending_.frame_callbacks.push_back(callback); }
    void set_pending_opaque_region(const Region* region);
    void set_pending_input_region(const Region* region);
    void set_pending_buffer_scale(int scale);
    void set_pending_buffer_transform(int transform);
    void add_pending_offset(int dx, int dy);
    /// wp_viewport: source crop in buffer coordinates (negative width = unset)
    /// and destination size in surface coordinates (-1 = unset).
    void set_pending_viewport_source(double x, double y, double w, double h);
    void set_pending_viewport_destination(int w, int h);
    void apply_commit();
    /// Called from the buffer's destroy listener: the client threw the buffer
    /// away while we still referenced it. Drops it from every slot so nothing
    /// releases or reads a dead resource. (Clients really do this — a toolkit
    /// discards its whole swapchain when the window is resized or re-shown.)
    void forget_buffer(wl_resource* buffer) noexcept;

    // --- internal: called by the wl_subsurface glue in subcompositor.cpp ---
    /// Become a subsurface of `parent` (placed on top of it). Returns false if
    /// `parent` is this surface or one of its descendants (a protocol error).
    [[nodiscard]] bool sub_attach(Surface& parent);
    void sub_detach();
    void sub_set_position(int x, int y) noexcept; // applied on the parent's commit
    void sub_set_sync(bool sync) noexcept;
    [[nodiscard]] bool sub_sync() const noexcept { return sub_sync_; }
    /// Restack relative to `sibling` (which may be the parent surface itself).
    /// Returns false if `sibling` isn't the parent or one of its children.
    [[nodiscard]] bool sub_place(Surface& sibling, bool above);

    /// A destroy subscription on one client buffer. Defined in compositor.cpp
    /// (it holds a libwayland `wl_listener`, which public headers can't name).
    struct BufferWatch;

private:
    /// wp_viewport state: crop the source, stretch to the destination.
    struct Viewport {
        bool has_source = false;
        double src_x = 0, src_y = 0, src_w = 0, src_h = 0;
        int dst_w = -1, dst_h = -1; // -1 = derive from the buffer
        [[nodiscard]] bool operator==(const Viewport&) const = default;
    };

    struct State {
        wl_resource* buffer = nullptr;
        bool buffer_dirty = false;
        bool tearing = false;
        std::vector<wl_resource*> frame_callbacks;
        std::vector<Box> damage;         // surface coordinates
        std::vector<Box> buffer_damage;  // buffer coordinates (damage_buffer)
        int scale = 1;
        Transform transform = Transform::normal;
        Region opaque;
        Region input;
        bool has_input = false;
        Viewport viewport;
        int offset_x = 0, offset_y = 0; // accumulated since the last commit
    };

    void watch_buffer(wl_resource* buffer);
    void prune_buffer_watches();
    void commit_state(State state);
    void parent_committed();
    void reset_pending(const State& applied);
    static void merge_state(State& into, State&& from);
    [[nodiscard]] bool effective_sync() const noexcept;
    void apply_pending_position() noexcept;
    [[nodiscard]] bool is_ancestor_of(const Surface& other) const noexcept;
    void collect_tree(std::vector<SurfaceAt>& out, int ox, int oy);

    wl_resource* resource_;
    wl_resource* current_buffer_ = nullptr;
    int buffer_width_ = 0;
    int buffer_height_ = 0;
    bool tearing_ = false;
    int buffer_scale_ = 1;
    Transform buffer_transform_ = Transform::normal; // buffer -> surface
    Region opaque_;
    Region input_;
    bool has_input_region_ = false;
    Viewport viewport_;
    int offset_x_ = 0, offset_y_ = 0;
    int acquire_fence_ = -1; // sync_file, owned

    // GPU texture cache. `texture_live_` marks a dmabuf import, which reads the
    // client's memory directly and so stays valid across commits; an upload is
    // a snapshot and has to be redone whenever the client draws again.
    std::optional<GpuTexture> texture_;
    wl_resource* texture_buffer_ = nullptr;
    VulkanRenderer* texture_renderer_ = nullptr;
    bool texture_live_ = false;
    std::vector<Box> damage_;
    std::vector<wl_resource*> queued_frame_callbacks_; // fired on presentation

    State pending_;         // accumulated since the last commit request
    State cached_;          // sync-mode state waiting for the parent's commit
    bool has_cached_ = false;
    // One entry per client buffer we still reference, so we hear about its
    // destruction before we touch a freed wl_resource.
    std::vector<std::unique_ptr<BufferWatch>> buffer_watches_;

    // Subsurface state. `parent_` is set only while a wl_subsurface exists.
    Surface* parent_ = nullptr;
    int sub_x_ = 0, sub_y_ = 0;
    int pending_sub_x_ = 0, pending_sub_y_ = 0;
    bool sub_position_dirty_ = false;
    bool sub_sync_ = true;
    std::vector<Surface*> below_; // children under the parent, back-to-front
    std::vector<Surface*> above_; // children over the parent, back-to-front
};

/// The Surface behind a `wl_surface` resource, or null if that resource is
/// something else. The one safe way for other protocol globals (tearing-control,
/// presentation-time, xdg-shell) to get from a client's object to ours.
[[nodiscard]] Surface* surface_from_resource(wl_resource* resource);

/// The wl_compositor global. Move-only; state is pointer-stable (pimpl) so the
/// libwayland global can safely hold a pointer to it.
class Compositor {
public:
    [[nodiscard]] static Result<Compositor> create(Display& display);

    ~Compositor();
    Compositor(Compositor&&) noexcept;
    Compositor& operator=(Compositor&&) noexcept;
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    /// Fires when a client creates a new wl_surface.
    [[nodiscard]] Signal<NewSurface>& new_surface() noexcept;

    struct Impl; // defined in compositor.cpp; opaque to users but named by the glue

private:
    std::unique_ptr<Impl> impl_;
    explicit Compositor(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
