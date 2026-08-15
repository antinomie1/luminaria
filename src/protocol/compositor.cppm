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

#include "detail/wayland_fwd.h"
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <vector>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unistd.h> // close
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

export module luminaria:compositor;

import :box;
import :client_buffer;
import :display;
import :dmabuf;
import :expected;
import :handle;
import :pixel_layout;
import :region;
import :signal;
import :transform;

export namespace luminaria {

class Display;
class Surface;

/// Stable identity for a Surface. Slots are reused, but their generation is
/// advanced first, so an id retained past client destruction never resolves to
/// a different client's later surface (the ABA case).
struct SurfaceId {
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;

    [[nodiscard]] bool valid() const noexcept {
        return index != std::numeric_limits<std::uint32_t>::max();
    }
    [[nodiscard]] bool operator==(const SurfaceId&) const noexcept = default;
};

struct NewSurface {
    Surface& surface;
};

struct SurfaceCommit {
    Surface& surface;
};

/// Fired just before a Surface is destroyed. Use for surface-local teardown;
/// long-lived references should retain SurfaceId and resolve it on each use.
struct SurfaceDestroy {
    Surface& surface;
};

/// Emitted after a SurfaceId has stopped resolving. Long-lived components may
/// listen once for behavioural cleanup; memory safety does not depend on it.
struct SurfaceInvalidated {
    SurfaceId surface;
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
    /// Fires just before this Surface goes away. This is for surface-local
    /// teardown; retain id() rather than this object's address across dispatch.
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
    /// True once this surface has been used as a pointer cursor
    /// (`wl_pointer.set_cursor`) or a drag icon. Both are drawn ON the pointer,
    /// and the protocol says their input region is ignored — so they must not
    /// take input, or the hit test under the pointer answers "the cursor" and
    /// the window beneath it never sees the pointer at all.
    [[nodiscard]] bool input_transparent() const noexcept { return input_transparent_; }
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
    [[nodiscard]] int acquire_fence_fd() const noexcept { return acquire_fence_.get(); }
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

    /// This surface's generational identity. Safe to retain across dispatch;
    /// resolve it with surface_from_id() each time it is used.
    [[nodiscard]] SurfaceId id() const noexcept { return id_; }

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

    /// A third bridge, for the case where the compositor draws nothing at all:
    /// if the committed buffer is a dmabuf, describe it so it can be handed
    /// straight to `Output::import_scanout()`. False for shm, single-pixel and
    /// no buffer. `out.fd` is borrowed and stays valid only while this same
    /// buffer is committed — see `DirectScanout`, which does the bookkeeping.
    [[nodiscard]] bool current_buffer_dmabuf(DmabufPlane& out) const;

    /// The committed wl_buffer, or null. Raw, and mainly here so a cache can be
    /// keyed on buffer identity; anything holding it MUST take a destroy
    /// listener, because the client can drop it at any time.
    [[nodiscard]] wl_resource* current_buffer() const noexcept { return current_buffer_; }

    // --- holding a buffer past its commit ---
    //
    // Normally a buffer is released the moment the next one is committed: the
    // renderer has already sampled it. Direct scanout breaks that promise — the
    // display hardware reads the client's buffer for as long as it is on
    // screen, so telling the client it may draw into it again puts a half-drawn
    // frame on the monitor. `hold_buffer` defers the release until whoever put
    // the buffer on screen says it is off again.

    /// Suppress `wl_buffer.release` for `buffer` until `unhold_buffer`. Holding
    /// the same buffer twice is one hold. Null and unknown buffers are ignored.
    void hold_buffer(wl_resource* buffer);

    /// Drop a hold, sending the release that was withheld if the buffer is no
    /// longer the committed one. Safe to call for a buffer that was never held.
    void unhold_buffer(wl_resource* buffer);

    // --- subsurface tree ---

    /// This surface's parent, if it is a subsurface.
    [[nodiscard]] Surface* subsurface_parent() const noexcept { return parent_; }
    /// Position relative to the parent surface (0,0 if not a subsurface).
    [[nodiscard]] int subsurface_x() const noexcept { return sub_x_; }
    [[nodiscard]] int subsurface_y() const noexcept { return sub_y_; }

    /// This surface plus all its subsurfaces, back-to-front, with offsets
    /// relative to *this* surface. Always contains at least `{this, 0, 0}`.
    [[nodiscard]] std::vector<SurfaceAt> surface_tree();

    /// The same, appended to a vector the caller owns and reuses. This is the
    /// one a per-frame loop wants: the returning overload allocates a fresh
    /// vector for every window, every frame, and the shell layer's budget for
    /// that is zero.
    void surface_tree(std::vector<SurfaceAt>& out);

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
    /// Called by the seat (cursor) and the data device (drag icon) when this
    /// surface takes on a role that rides the pointer. One-way: a role is
    /// permanent for the life of the surface.
    void set_input_transparent() noexcept { input_transparent_ = true; }
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
    SurfaceId id_;
    wl_resource* current_buffer_ = nullptr;
    int buffer_width_ = 0;
    int buffer_height_ = 0;
    bool tearing_ = false;
    int buffer_scale_ = 1;
    Transform buffer_transform_ = Transform::normal; // buffer -> surface
    Region opaque_;
    Region input_;
    bool has_input_region_ = false;
    bool input_transparent_ = false; // cursor / drag-icon role
    Viewport viewport_;
    // Buffers someone is scanning out, and the releases that are waiting on
    // them. Both are tiny — a client's swapchain is two or three buffers.
    std::vector<wl_resource*> held_buffers_;
    std::vector<wl_resource*> release_due_;
    int offset_x_ = 0, offset_y_ = 0;
    UniqueFd acquire_fence_; // sync_file, owned

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

/// Resolve a generational id, or null once that exact Surface has gone away.
[[nodiscard]] Surface* surface_from_id(SurfaceId id) noexcept;

/// Process-wide invalidation stream for generational surface identities.
[[nodiscard]] Signal<SurfaceInvalidated>& surface_invalidated() noexcept;

/// The Region behind a `wl_region` resource (null in, null out). Protocols that
/// take a wl_region of their own — pointer-constraints' confinement area — read
/// it through this rather than re-implementing wl_region.
[[nodiscard]] Region* region_from_resource(wl_resource* resource);

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

// --------------------------------------------------------------- implementation
namespace luminaria {

namespace {

struct SurfaceSlot {
    Surface* surface = nullptr;
    std::uint32_t generation = 1;
};

struct SurfaceRegistry {
    std::vector<SurfaceSlot> slots;
    std::vector<std::uint32_t> free;
};

SurfaceRegistry& surface_registry() {
    static SurfaceRegistry registry;
    return registry;
}

SurfaceId register_surface(Surface* surface) noexcept {
    try {
        SurfaceRegistry& registry = surface_registry();
        if (!registry.free.empty()) {
            const std::uint32_t index = registry.free.back();
            registry.free.pop_back();
            SurfaceSlot& slot = registry.slots[index];
            slot.surface = surface;
            return SurfaceId{index, slot.generation};
        }
        if (registry.slots.size() >= std::numeric_limits<std::uint32_t>::max()) {
            return {};
        }
        const auto index = static_cast<std::uint32_t>(registry.slots.size());
        registry.slots.push_back(SurfaceSlot{surface, 1});
        return SurfaceId{index, 1};
    } catch (...) {
        return {};
    }
}

void unregister_surface(SurfaceId id) noexcept {
    if (!id.valid()) {
        return;
    }
    SurfaceRegistry& registry = surface_registry();
    if (id.index >= registry.slots.size()) {
        return;
    }
    SurfaceSlot& slot = registry.slots[id.index];
    if (slot.surface == nullptr || slot.generation != id.generation) {
        return;
    }
    slot.surface = nullptr;
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    try {
        registry.free.push_back(id.index);
    } catch (...) {
        // Losing a free-list entry only prevents this slot being reused. The
        // generation was already advanced, so stale ids remain harmless.
    }
}

} // namespace

Surface* surface_from_id(SurfaceId id) noexcept {
    if (!id.valid()) {
        return nullptr;
    }
    SurfaceRegistry& registry = surface_registry();
    if (id.index >= registry.slots.size()) {
        return nullptr;
    }
    const SurfaceSlot& slot = registry.slots[id.index];
    return slot.generation == id.generation ? slot.surface : nullptr;
}

Signal<SurfaceInvalidated>& surface_invalidated() noexcept {
    static Signal<SurfaceInvalidated> signal;
    return signal;
}

struct Compositor::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    Signal<NewSurface> new_surface;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

void Surface::hold_buffer(wl_resource* buffer) {
    if (buffer == nullptr ||
        std::find(held_buffers_.begin(), held_buffers_.end(), buffer) != held_buffers_.end()) {
        return;
    }
    held_buffers_.push_back(buffer);
    // The hold outlives the commit that installed the buffer, so the watch has
    // to outlive it too: prune_buffer_watches() only keeps pending/cached/current.
    watch_buffer(buffer);
}

void Surface::unhold_buffer(wl_resource* buffer) {
    if (std::erase(held_buffers_, buffer) == 0) {
        return;
    }
    // The release was withheld while the buffer was on screen. Now it is not,
    // and the client is free to draw into it again.
    if (std::erase(release_due_, buffer) != 0) {
        wl_buffer_send_release(buffer);
    }
    prune_buffer_watches();
}

bool Surface::current_buffer_dmabuf(DmabufPlane& out) const {
    ClientBuffer* buffer = client_buffer_from_resource(current_buffer_);
    return buffer != nullptr && buffer->dmabuf(out);
}

bool Surface::current_buffer_rgba(std::vector<std::uint8_t>& out, int& width, int& height) const {
    if (current_buffer_ == nullptr) {
        return false;
    }
    wl_shm_buffer* shm = wl_shm_buffer_get(current_buffer_);
    if (shm == nullptr) {
        ClientBuffer* buffer = client_buffer_from_resource(current_buffer_);
        return buffer != nullptr && buffer->rgba(out, width, height);
    }
    const uint32_t format = wl_shm_buffer_get_format(shm);
    if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888) {
        return false;
    }
    const int w = wl_shm_buffer_get_width(shm);
    const int h = wl_shm_buffer_get_height(shm);
    const int stride = wl_shm_buffer_get_stride(shm);
    const bool opaque = format == WL_SHM_FORMAT_XRGB8888;
    // libwayland accepted this buffer having only checked `stride >= width`,
    // comparing bytes against pixels. A client that declares stride == width
    // for a 4-byte format sends the loop below off the end of the pool, so the
    // buffer is unreadable to us — say so rather than touch it. libwayland
    // guarantees the pool holds `stride * height`, which is all we then need.
    if (!layout_fits(w, h, stride)) {
        return false;
    }

    wl_shm_buffer_begin_access(shm);
    const auto* data = static_cast<const uint8_t*>(wl_shm_buffer_get_data(shm));
    out.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = data + static_cast<size_t>(y) * stride;
        uint8_t* dst = out.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            // shm ARGB8888 is little-endian: bytes are B,G,R,A. Emit RGBA.
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = opaque ? 255 : src[x * 4 + 3];
        }
    }
    wl_shm_buffer_end_access(shm);
    width = w;
    height = h;
    return true;
}

namespace {
// Buffer extent without decoding pixels: shm knows it directly, dmabuf carries
// it in the plane metadata. Used for hit-testing and subsurface layout.
void buffer_size(wl_resource* buffer, int& w, int& h) {
    w = 0;
    h = 0;
    if (buffer == nullptr) {
        return;
    }
    if (wl_shm_buffer* shm = wl_shm_buffer_get(buffer); shm != nullptr) {
        w = wl_shm_buffer_get_width(shm);
        h = wl_shm_buffer_get_height(shm);
        return;
    }
    if (ClientBuffer* contents = client_buffer_from_resource(buffer); contents != nullptr) {
        w = contents->width();
        h = contents->height();
        return;
    }
}
} // namespace

// ---- client buffer lifetime -------------------------------------------------
//
// A wl_buffer belongs to the CLIENT, and clients destroy buffers while the
// compositor still holds them: every toolkit drops its whole swapchain when a
// window is resized, hidden, or re-shown. Keeping the raw wl_resource* means
// the next `wl_buffer.release` — or the next readback — touches freed memory.
// So every buffer we reference carries a destroy subscription.

struct Surface::BufferWatch {
    wl_listener listener{}; // must stay first: we recover the watch from it
    Surface* surface = nullptr;
    wl_resource* buffer = nullptr;

    ~BufferWatch() {
        // After libwayland's final emit the link is re-initialised, so removing
        // it again is harmless; before we ever attached it, it is null.
        if (listener.link.prev != nullptr) {
            wl_list_remove(&listener.link);
        }
    }
};

namespace {
void on_buffer_destroy(wl_listener* listener, void*) {
    auto* watch = reinterpret_cast<Surface::BufferWatch*>(
        reinterpret_cast<char*>(listener) - offsetof(Surface::BufferWatch, listener));
    watch->surface->forget_buffer(watch->buffer);
}
} // namespace

void Surface::watch_buffer(wl_resource* buffer) {
    if (buffer == nullptr) {
        return;
    }
    for (const std::unique_ptr<BufferWatch>& watch : buffer_watches_) {
        if (watch->buffer == buffer) {
            return; // already subscribed
        }
    }
    auto watch = std::make_unique<BufferWatch>();
    watch->surface = this;
    watch->buffer = buffer;
    watch->listener.notify = on_buffer_destroy;
    wl_resource_add_destroy_listener(buffer, &watch->listener);
    buffer_watches_.push_back(std::move(watch));
}

void Surface::prune_buffer_watches() {
    std::erase_if(buffer_watches_, [this](const std::unique_ptr<BufferWatch>& watch) {
        return watch->buffer != current_buffer_ && watch->buffer != pending_.buffer &&
               watch->buffer != cached_.buffer &&
               std::find(held_buffers_.begin(), held_buffers_.end(), watch->buffer) ==
                   held_buffers_.end();
    });
}

void Surface::forget_buffer(wl_resource* buffer) noexcept {
    // The client destroyed it out from under every hold. There is nothing left
    // to release and nothing left to hold.
    std::erase(held_buffers_, buffer);
    std::erase(release_due_, buffer);
    // A pending/cached attach of a now-dead buffer degrades to "attach null",
    // which is exactly what the surface should show: nothing.
    if (pending_.buffer == buffer) {
        pending_.buffer = nullptr;
    }
    if (cached_.buffer == buffer) {
        cached_.buffer = nullptr;
    }
    if (current_buffer_ == buffer) {
        current_buffer_ = nullptr;
        buffer_width_ = 0;
        buffer_height_ = 0;
    }
    std::erase_if(buffer_watches_, [buffer](const std::unique_ptr<BufferWatch>& watch) {
        return watch->buffer == buffer;
    });
}

void Surface::set_pending_buffer(wl_resource* buffer) {
    pending_.buffer = buffer;
    pending_.buffer_dirty = true;
    watch_buffer(buffer);
}

void Surface::add_pending_damage(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    pending_.damage.push_back(Box{x, y, width, height});
}

void Surface::add_pending_buffer_damage(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    pending_.buffer_damage.push_back(Box{x, y, width, height});
}

void Surface::set_pending_opaque_region(const Region* region) {
    pending_.opaque = region != nullptr ? *region : Region{};
}

void Surface::set_pending_input_region(const Region* region) {
    // A null region is the protocol's "infinite": the whole surface takes input.
    pending_.has_input = region != nullptr;
    pending_.input = region != nullptr ? *region : Region{};
}

void Surface::set_pending_buffer_scale(int scale) {
    pending_.scale = scale < 1 ? 1 : scale;
}

void Surface::set_pending_buffer_transform(int transform) {
    if (transform < 0 || transform > 7) {
        return;
    }
    // The client reports the transform it PRE-APPLIED to match an output; what
    // we need to undo it is the inverse, and that is what we store throughout.
    pending_.transform = transform_invert(static_cast<Transform>(transform));
}

void Surface::add_pending_offset(int dx, int dy) {
    pending_.offset_x += dx;
    pending_.offset_y += dy;
}

void Surface::set_pending_viewport_source(double x, double y, double w, double h) {
    pending_.viewport.has_source = w > 0 && h > 0;
    pending_.viewport.src_x = x;
    pending_.viewport.src_y = y;
    pending_.viewport.src_w = w;
    pending_.viewport.src_h = h;
}

void Surface::set_pending_viewport_destination(int w, int h) {
    pending_.viewport.dst_w = w;
    pending_.viewport.dst_h = h;
}

void Surface::set_acquire_fence(int fd) noexcept {
    acquire_fence_.reset(fd);
}

void Surface::notify_rendered(int fence_fd) {
    SurfaceRendered event{*this, fence_fd};
    rendered.emit(event);
}

void Surface::set_preferred_buffer_scale(int scale) {
    if (wl_resource_get_version(resource_) >= WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION) {
        wl_surface_send_preferred_buffer_scale(resource_, scale < 1 ? 1 : scale);
    }
}

void Surface::set_preferred_buffer_transform(Transform transform) {
    if (wl_resource_get_version(resource_) >= WL_SURFACE_PREFERRED_BUFFER_TRANSFORM_SINCE_VERSION) {
        wl_surface_send_preferred_buffer_transform(resource_,
                                                   static_cast<uint32_t>(transform));
    }
}

namespace {
/// Buffer pixels -> surface coordinates. `buffer_to_surface` maps the buffer's
/// orientation onto the surface's; the scale then divides, rounded outward so a
/// damage rect never shrinks below what actually changed.
Box buffer_box_to_surface(const Box& b, Transform buffer_to_surface, int scale, int surface_w,
                          int surface_h) {
    const Box rotated =
        transform_box(buffer_to_surface, 1, b, surface_w * scale, surface_h * scale);
    const int x0 = rotated.x / scale;
    const int y0 = rotated.y / scale;
    const int x1 = (rotated.x + rotated.width + scale - 1) / scale;
    const int y1 = (rotated.y + rotated.height + scale - 1) / scale;
    return Box{x0, y0, x1 - x0, y1 - y0};
}
} // namespace

int Surface::surface_width() const noexcept {
    if (viewport_.dst_w >= 0) {
        return viewport_.dst_w;
    }
    if (viewport_.has_source) {
        return static_cast<int>(std::ceil(viewport_.src_w));
    }
    const int w = transform_swaps_axes(buffer_transform_) ? buffer_height_ : buffer_width_;
    return w / buffer_scale_;
}

int Surface::surface_height() const noexcept {
    if (viewport_.dst_h >= 0) {
        return viewport_.dst_h;
    }
    if (viewport_.has_source) {
        return static_cast<int>(std::ceil(viewport_.src_h));
    }
    const int h = transform_swaps_axes(buffer_transform_) ? buffer_width_ : buffer_height_;
    return h / buffer_scale_;
}

namespace {
/// transform_box's rules on the unit square, in floats — for normalized texture
/// coordinates, where the integer version would round a crop to nothing.
void unit_transform(Transform t, float& x0, float& y0, float& x1, float& y1) noexcept {
    if (transform_flipped(t)) {
        const float nx0 = 1.0f - x1, nx1 = 1.0f - x0;
        x0 = nx0;
        x1 = nx1;
    }
    const float a = x0, b = y0, c = x1, d = y1;
    switch (transform_rotation(t)) {
    case 90:
        x0 = 1.0f - d;
        y0 = a;
        x1 = 1.0f - b;
        y1 = c;
        break;
    case 180:
        x0 = 1.0f - c;
        y0 = 1.0f - d;
        x1 = 1.0f - a;
        y1 = 1.0f - b;
        break;
    case 270:
        x0 = b;
        y0 = 1.0f - c;
        x1 = d;
        y1 = 1.0f - a;
        break;
    default:
        break;
    }
}
} // namespace

void Surface::buffer_source_uv(float& u0, float& v0, float& u1, float& v1) const noexcept {
    u0 = v0 = 0.0f;
    u1 = v1 = 1.0f;
    if (!viewport_.has_source || buffer_width_ <= 0 || buffer_height_ <= 0) {
        return;
    }
    // The crop is given in the buffer's coordinates *after* transform and scale.
    // Normalizing divides the scale out, so only the rotation is left to undo.
    const bool swap = transform_swaps_axes(buffer_transform_);
    const double base_w = static_cast<double>(swap ? buffer_height_ : buffer_width_) / buffer_scale_;
    const double base_h = static_cast<double>(swap ? buffer_width_ : buffer_height_) / buffer_scale_;
    if (base_w <= 0 || base_h <= 0) {
        return;
    }
    u0 = static_cast<float>(viewport_.src_x / base_w);
    v0 = static_cast<float>(viewport_.src_y / base_h);
    u1 = static_cast<float>((viewport_.src_x + viewport_.src_w) / base_w);
    v1 = static_cast<float>((viewport_.src_y + viewport_.src_h) / base_h);
    unit_transform(transform_invert(buffer_transform_), u0, v0, u1, v1);
}

bool Surface::accepts_input(double sx, double sy) const noexcept {
    if (input_transparent_) {
        return false;
    }
    if (sx < 0 || sy < 0 || sx >= surface_width() || sy >= surface_height()) {
        return false;
    }
    // No region set means the protocol's "infinite" region: everything.
    return !has_input_region_ ||
           input_.contains(static_cast<int>(sx), static_cast<int>(sy));
}

void Surface::send_frame_done(std::uint32_t time_ms) {
    for (wl_resource* cb : queued_frame_callbacks_) {
        wl_callback_send_done(cb, time_ms);
        wl_resource_destroy(cb);
    }
    queued_frame_callbacks_.clear();
}

Surface::Surface(wl_resource* resource) noexcept : resource_(resource), id_(register_surface(this)) {}

Surface::~Surface() {
    SurfaceDestroy event{*this};
    destroy.emit(event);
    // Callbacks the client is still waiting on will never fire; the protocol
    // says to destroy them rather than leave the client hanging.
    for (wl_resource* cb : queued_frame_callbacks_) {
        wl_resource_destroy(cb);
    }
    queued_frame_callbacks_.clear();
    // Orphan our children and unlink from our parent so no dangling edges remain.
    for (Surface* child : below_) {
        child->parent_ = nullptr;
    }
    for (Surface* child : above_) {
        child->parent_ = nullptr;
    }
    if (parent_ != nullptr) {
        std::erase(parent_->below_, this);
        std::erase(parent_->above_, this);
        parent_ = nullptr;
    }
    const SurfaceId dead = id_;
    unregister_surface(dead);
    SurfaceInvalidated invalidated{dead};
    surface_invalidated().emit(invalidated);
}

bool Surface::effective_sync() const noexcept {
    for (const Surface* s = this; s->parent_ != nullptr; s = s->parent_) {
        if (s->sub_sync_) {
            return true;
        }
    }
    return false;
}

void Surface::merge_state(State& into, State&& from) {
    if (from.buffer_dirty) {
        // A buffer superseded before it was ever applied is released right away.
        if (into.buffer_dirty && into.buffer != nullptr && into.buffer != from.buffer) {
            wl_buffer_send_release(into.buffer);
        }
        into.buffer = from.buffer;
        into.buffer_dirty = true;
    }
    into.frame_callbacks.insert(into.frame_callbacks.end(), from.frame_callbacks.begin(),
                                from.frame_callbacks.end());
    from.frame_callbacks.clear();
    into.damage.insert(into.damage.end(), from.damage.begin(), from.damage.end());
    from.damage.clear();
    into.buffer_damage.insert(into.buffer_damage.end(), from.buffer_damage.begin(),
                              from.buffer_damage.end());
    from.buffer_damage.clear();
    into.tearing = from.tearing;
    into.scale = from.scale;
    into.transform = from.transform;
    into.opaque = std::move(from.opaque);
    into.input = std::move(from.input);
    into.has_input = from.has_input;
    into.viewport = from.viewport;
    into.offset_x += from.offset_x;
    into.offset_y += from.offset_y;
}

// Most double-buffered state is *sticky*: scale, transform, the regions and the
// viewport keep their value until the client changes them, so the fresh pending
// state starts from what was just applied rather than from the defaults. Damage,
// frame callbacks, the buffer and the offset are per-commit and do reset.
void Surface::reset_pending(const State& applied) {
    pending_ = State{};
    pending_.tearing = applied.tearing;
    pending_.scale = applied.scale;
    pending_.transform = applied.transform;
    pending_.opaque = applied.opaque;
    pending_.input = applied.input;
    pending_.has_input = applied.has_input;
    pending_.viewport = applied.viewport;
}

void Surface::apply_commit() {
    if (parent_ != nullptr && effective_sync()) {
        // Sync subsurface: state is cached until the parent commits.
        merge_state(cached_, std::move(pending_));
        has_cached_ = true;
        reset_pending(cached_);
        prune_buffer_watches();
        return;
    }
    State state = std::move(pending_);
    reset_pending(state);
    commit_state(std::move(state));
}

void Surface::commit_state(State state) {
    const int old_w = surface_width();
    const int old_h = surface_height();
    if (state.buffer_dirty) {
        // Release the buffer we're replacing so the client can reuse it. We copy
        // buffer contents at render time, so we only need to hold the current one
        // — unless someone put it on the screen directly, in which case the
        // release waits for their unhold_buffer().
        if (current_buffer_ != nullptr && current_buffer_ != state.buffer) {
            if (std::find(held_buffers_.begin(), held_buffers_.end(), current_buffer_) !=
                held_buffers_.end()) {
                release_due_.push_back(current_buffer_);
            } else {
                wl_buffer_send_release(current_buffer_);
            }
        }
        current_buffer_ = state.buffer;
        buffer_size(current_buffer_, buffer_width_, buffer_height_);
    }
    tearing_ = state.tearing;
    buffer_scale_ = state.scale;
    buffer_transform_ = state.transform;
    opaque_ = state.opaque;
    input_ = state.input;
    has_input_region_ = state.has_input;
    viewport_ = state.viewport;
    offset_x_ += state.offset_x;
    offset_y_ += state.offset_y;

    const int new_w = surface_width();
    const int new_h = surface_height();
    // A resize invalidates everything; so does a new buffer the client didn't
    // bother to describe. Otherwise take the client's word, clipped to the
    // surface — a lying damage rect must not make us read outside the buffer.
    const bool resized = new_w != old_w || new_h != old_h;
    const bool described = !state.damage.empty() || !state.buffer_damage.empty();
    if (resized || (state.buffer_dirty && !described)) {
        damage_.assign(1, Box{0, 0, new_w, new_h});
    } else {
        const Box bounds{0, 0, new_w, new_h};
        for (const Box& b : state.damage) {
            if (const Box clipped = b.intersection(bounds); !clipped.empty()) {
                damage_.push_back(clipped);
            }
        }
        // damage_buffer arrives in buffer pixels: undo the transform and scale
        // so everything downstream sees one coordinate space. A viewport makes
        // that mapping non-uniform (crop + stretch), and chasing it is not worth
        // it for a hint — repaint the surface instead.
        if (!state.buffer_damage.empty()) {
            const bool swap = transform_swaps_axes(buffer_transform_);
            const int base_w = (swap ? buffer_height_ : buffer_width_) / buffer_scale_;
            const int base_h = (swap ? buffer_width_ : buffer_height_) / buffer_scale_;
            if (viewport_.has_source || viewport_.dst_w >= 0) {
                damage_.push_back(bounds);
            } else {
                for (const Box& b : state.buffer_damage) {
                    const Box surf = buffer_box_to_surface(b, buffer_transform_, buffer_scale_,
                                                           base_w, base_h);
                    if (const Box clipped = surf.intersection(bounds); !clipped.empty()) {
                        damage_.push_back(clipped);
                    }
                }
            }
        }
    }
    // Regions are surface state, so clip them once here rather than at every use.
    opaque_.intersect(Box{0, 0, new_w, new_h});
    input_.intersect(Box{0, 0, new_w, new_h});

    prune_buffer_watches();
    SurfaceCommit event{*this};
    commit.emit(event);

    // Frame callbacks are NOT acked here: a client that draws on commit-ack runs
    // as fast as the CPU allows and tears through frames the display never
    // showed. They wait for send_frame_done(), which the compositor calls when
    // the frame is actually on screen.
    queued_frame_callbacks_.insert(queued_frame_callbacks_.end(), state.frame_callbacks.begin(),
                                   state.frame_callbacks.end());

    // A parent commit applies the whole synced subtree, including subsurface
    // positions (which are parent state).
    const std::vector<Surface*> below = below_;
    const std::vector<Surface*> above = above_;
    for (Surface* child : below) {
        child->parent_committed();
    }
    for (Surface* child : above) {
        child->parent_committed();
    }
}

void Surface::parent_committed() {
    apply_pending_position();
    if (has_cached_) {
        State state = std::move(cached_);
        cached_ = State{};
        has_cached_ = false;
        commit_state(std::move(state)); // recurses into our own children
        return;
    }
    const std::vector<Surface*> below = below_;
    const std::vector<Surface*> above = above_;
    for (Surface* child : below) {
        child->parent_committed();
    }
    for (Surface* child : above) {
        child->parent_committed();
    }
}

void Surface::apply_pending_position() noexcept {
    if (sub_position_dirty_) {
        sub_x_ = pending_sub_x_;
        sub_y_ = pending_sub_y_;
        sub_position_dirty_ = false;
    }
}

bool Surface::is_ancestor_of(const Surface& other) const noexcept {
    for (const Surface* s = other.parent_; s != nullptr; s = s->parent_) {
        if (s == this) {
            return true;
        }
    }
    return false;
}

bool Surface::sub_attach(Surface& parent) {
    if (&parent == this || is_ancestor_of(parent)) {
        return false; // would make a cycle
    }
    sub_detach();
    parent_ = &parent;
    parent.above_.push_back(this);
    sub_sync_ = true;
    return true;
}

void Surface::sub_detach() {
    if (parent_ == nullptr) {
        return;
    }
    std::erase(parent_->below_, this);
    std::erase(parent_->above_, this);
    parent_ = nullptr;
    sub_x_ = sub_y_ = 0;
    sub_position_dirty_ = false;
    // Cached sync state has no parent to release it any more — apply it now.
    if (has_cached_) {
        State state = std::move(cached_);
        cached_ = State{};
        has_cached_ = false;
        commit_state(std::move(state));
    }
}

void Surface::sub_set_position(int x, int y) noexcept {
    pending_sub_x_ = x;
    pending_sub_y_ = y;
    sub_position_dirty_ = true;
}

void Surface::sub_set_sync(bool sync) noexcept {
    sub_sync_ = sync;
    if (!sync && has_cached_ && !effective_sync()) {
        State state = std::move(cached_);
        cached_ = State{};
        has_cached_ = false;
        commit_state(std::move(state));
    }
}

bool Surface::sub_place(Surface& sibling, bool above) {
    if (parent_ == nullptr) {
        return false;
    }
    Surface* parent = parent_;
    std::vector<Surface*>& target = above ? parent->above_ : parent->below_;
    if (&sibling == parent) {
        // Relative to the parent itself: move into the above/below list, at the
        // edge nearest the parent.
        std::erase(parent->below_, this);
        std::erase(parent->above_, this);
        if (above) {
            target.insert(target.begin(), this);
        } else {
            target.push_back(this);
        }
        return true;
    }
    if (sibling.parent_ != parent) {
        return false;
    }
    std::erase(parent->below_, this);
    std::erase(parent->above_, this);
    for (std::vector<Surface*>* list : {&parent->below_, &parent->above_}) {
        auto it = std::find(list->begin(), list->end(), &sibling);
        if (it != list->end()) {
            list->insert(above ? it + 1 : it, this);
            return true;
        }
    }
    return false; // sibling vanished between the checks; nothing sensible to do
}

void Surface::collect_tree(std::vector<SurfaceAt>& out, int ox, int oy) {
    // Each node carries its own wl_surface.offset, so a client that shifts its
    // content moves without the compositor having to know why.
    ox += offset_x_;
    oy += offset_y_;
    for (Surface* child : below_) {
        child->collect_tree(out, ox + child->sub_x_, oy + child->sub_y_);
    }
    out.push_back(SurfaceAt{this, ox, oy});
    for (Surface* child : above_) {
        child->collect_tree(out, ox + child->sub_x_, oy + child->sub_y_);
    }
}

std::vector<SurfaceAt> Surface::surface_tree() {
    std::vector<SurfaceAt> out;
    collect_tree(out, 0, 0);
    return out;
}

void Surface::surface_tree(std::vector<SurfaceAt>& out) { collect_tree(out, 0, 0); }

std::optional<SurfaceAt> Surface::surface_at(double sx, double sy) {
    const std::vector<SurfaceAt> tree = surface_tree();
    for (auto it = tree.rbegin(); it != tree.rend(); ++it) { // front-to-back
        const Surface* s = it->surface;
        if (s->buffer_width_ <= 0 || s->buffer_height_ <= 0) {
            continue;
        }
        // The input region, not the buffer rectangle: a client with rounded
        // corners or a drop shadow expects clicks in the transparent parts to
        // fall through to whatever is behind it.
        if (s->accepts_input(sx - it->x, sy - it->y)) {
            return *it;
        }
    }
    return std::nullopt;
}

namespace {

Surface* surface_of(wl_resource* resource) {
    return static_cast<Surface*>(wl_resource_get_user_data(resource));
}

// wl_region user data. libwayland already type-checks the argument against the
// wl_region interface, so the cast needs no guard of its own.
Region* region_of(wl_resource* resource) {
    return resource == nullptr ? nullptr
                               : static_cast<Region*>(wl_resource_get_user_data(resource));
}

void surface_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void surface_attach(wl_client*, wl_resource* resource, wl_resource* buffer, int32_t x, int32_t y) {
    Surface* surface = surface_of(resource);
    surface->set_pending_buffer(buffer);
    // Before v5 the offset rode along on attach; from v5 it must be zero and
    // clients use wl_surface.offset instead.
    if (x != 0 || y != 0) {
        if (wl_resource_get_version(resource) >= WL_SURFACE_OFFSET_SINCE_VERSION) {
            wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
                                   "wl_surface.attach offset must be zero since version 5");
            return;
        }
        surface->add_pending_offset(x, y);
    }
}
void surface_commit(wl_client*, wl_resource* resource) {
    surface_of(resource)->apply_commit();
}
void surface_frame(wl_client* client, wl_resource* resource, uint32_t callback_id) {
    wl_resource* cb = wl_resource_create(client, &wl_callback_interface, 1, callback_id);
    if (cb != nullptr) {
        surface_of(resource)->add_frame_callback(cb);
    }
}
// wl_surface.damage is in surface coordinates; damage_buffer is in buffer
// pixels and goes back through the buffer scale/transform on commit.
void surface_damage(wl_client*, wl_resource* resource, int32_t x, int32_t y, int32_t width,
                    int32_t height) {
    surface_of(resource)->add_pending_damage(x, y, width, height);
}
void surface_damage_buffer(wl_client*, wl_resource* resource, int32_t x, int32_t y, int32_t width,
                           int32_t height) {
    surface_of(resource)->add_pending_buffer_damage(x, y, width, height);
}
void surface_set_opaque_region(wl_client*, wl_resource* resource, wl_resource* region) {
    surface_of(resource)->set_pending_opaque_region(region_of(region));
}
void surface_set_input_region(wl_client*, wl_resource* resource, wl_resource* region) {
    surface_of(resource)->set_pending_input_region(region_of(region));
}
void surface_set_buffer_transform(wl_client*, wl_resource* resource, int32_t transform) {
    if (transform < 0 || transform > 7) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_TRANSFORM,
                               "unknown buffer transform %d", transform);
        return;
    }
    surface_of(resource)->set_pending_buffer_transform(transform);
}
void surface_set_buffer_scale(wl_client*, wl_resource* resource, int32_t scale) {
    if (scale < 1) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SCALE,
                               "buffer scale must be positive, got %d", scale);
        return;
    }
    surface_of(resource)->set_pending_buffer_scale(scale);
}
void surface_offset(wl_client*, wl_resource* resource, int32_t x, int32_t y) {
    surface_of(resource)->add_pending_offset(x, y);
}

constexpr struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy_request,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
};

void surface_resource_destroy(wl_resource* resource) {
    delete surface_of(resource);
}

void compositor_create_surface(wl_client* client, wl_resource* compositor_resource, uint32_t id) {
    auto* impl = static_cast<Compositor::Impl*>(wl_resource_get_user_data(compositor_resource));
    wl_resource* resource = wl_resource_create(
        client, &wl_surface_interface, wl_resource_get_version(compositor_resource), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* surface = new (std::nothrow) Surface{resource};
    if (surface == nullptr || !surface->id().valid()) {
        delete surface;
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &surface_impl, surface, surface_resource_destroy);

    NewSurface event{*surface};
    impl->new_surface.emit(event);
}

// wl_region: a real set of boxes. Clients build input and opaque regions out of
// add/subtract, and both now decide things — where clicks land and what the
// renderer can skip drawing.
void region_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void region_add(wl_client*, wl_resource* resource, int32_t x, int32_t y, int32_t w, int32_t h) {
    region_of(resource)->add(Box{x, y, w, h});
}
void region_subtract(wl_client*, wl_resource* resource, int32_t x, int32_t y, int32_t w,
                     int32_t h) {
    region_of(resource)->subtract(Box{x, y, w, h});
}
constexpr struct wl_region_interface region_impl = {
    .destroy = region_destroy_request,
    .add = region_add,
    .subtract = region_subtract,
};
void region_resource_destroy(wl_resource* resource) {
    delete region_of(resource);
}

void compositor_create_region(wl_client* client, wl_resource* compositor_resource, uint32_t id) {
    wl_resource* resource = wl_resource_create(
        client, &wl_region_interface, wl_resource_get_version(compositor_resource), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &region_impl, new Region{}, region_resource_destroy);
}

constexpr struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

void compositor_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_compositor_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &compositor_impl, data, nullptr);
}

} // namespace

Compositor::Compositor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Compositor::~Compositor() = default;
Compositor::Compositor(Compositor&&) noexcept = default;
Compositor& Compositor::operator=(Compositor&&) noexcept = default;

Result<Compositor> Compositor::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    // Version 6: wl_surface.offset (v5) plus preferred_buffer_scale /
    // preferred_buffer_transform (v6), which is how a HiDPI-aware client learns
    // what density to render at without inferring it from wl_output.
    impl->global = wl_global_create(impl->display, &wl_compositor_interface, 6, impl.get(),
                                    compositor_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wl_compositor) failed");
    }
    return Compositor{std::move(impl)};
}

Signal<NewSurface>& Compositor::new_surface() noexcept {
    return impl_->new_surface;
}

Surface* surface_from_resource(wl_resource* resource) {
    if (resource == nullptr ||
        !wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl)) {
        return nullptr;
    }
    return static_cast<Surface*>(wl_resource_get_user_data(resource));
}

Region* region_from_resource(wl_resource* resource) {
    if (resource == nullptr ||
        !wl_resource_instance_of(resource, &wl_region_interface, &region_impl)) {
        return nullptr;
    }
    return static_cast<Region*>(wl_resource_get_user_data(resource));
}

} // namespace luminaria
