// luminaria/compositor.hpp — the wl_compositor global and the wl_surface objects it
// mints. This is where clients' windows enter the compositor.
//
// A wl_surface can also be a *subsurface* of another (wl_subcompositor, see
// luminaria/subcompositor.hpp): a child positioned relative to its parent and
// stacked below or above it. `surface_tree()` walks a surface and its
// subsurfaces back-to-front, which is what the renderer and hit-testing want.
//
// Public header stays C-header-free: wl_resource is only forward-declared.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "luminaria/core/expected.hpp"
#include "luminaria/core/signal.hpp"

struct wl_resource; // opaque, from libwayland-server

namespace luminaria {

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

    /// True once a non-null buffer has been committed.
    [[nodiscard]] bool has_buffer() const noexcept { return current_buffer_ != nullptr; }

    /// Size of the committed buffer in pixels (0 if there is none).
    [[nodiscard]] int buffer_width() const noexcept { return buffer_width_; }
    [[nodiscard]] int buffer_height() const noexcept { return buffer_height_; }

    /// Escape hatch for protocol wiring (e.g. seat focus): the wl_surface resource.
    [[nodiscard]] wl_resource* c_resource() const noexcept { return resource_; }

    /// Copy the committed wl_shm buffer into tightly-packed RGBA8 (`out`), setting
    /// width/height. Returns false if there is no buffer or it isn't a supported
    /// shm format (ARGB8888/XRGB8888). This is the bridge into the renderer.
    [[nodiscard]] bool current_buffer_rgba(std::vector<std::uint8_t>& out, int& width,
                                           int& height) const;

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
    void add_frame_callback(wl_resource* callback) { pending_.frame_callbacks.push_back(callback); }
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
    struct State {
        wl_resource* buffer = nullptr;
        bool buffer_dirty = false;
        std::vector<wl_resource*> frame_callbacks;
    };

    void watch_buffer(wl_resource* buffer);
    void prune_buffer_watches();
    void commit_state(State state);
    void parent_committed();
    static void merge_state(State& into, State&& from);
    [[nodiscard]] bool effective_sync() const noexcept;
    void apply_pending_position() noexcept;
    [[nodiscard]] bool is_ancestor_of(const Surface& other) const noexcept;
    void collect_tree(std::vector<SurfaceAt>& out, int ox, int oy);

    wl_resource* resource_;
    wl_resource* current_buffer_ = nullptr;
    int buffer_width_ = 0;
    int buffer_height_ = 0;

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
