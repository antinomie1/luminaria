// luminaria/compositor.hpp — the wl_compositor global and the wl_surface objects it
// mints. This is where clients' windows enter the compositor.
//
// Public header stays C-header-free: wl_resource is only forward-declared.
#pragma once

#include <cstdint>
#include <memory>
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

/// A client wl_surface. Owned by its wl_resource (lives until the client
/// destroys it); its address is stable, so signals may capture `Surface&`.
class Surface {
    wl_resource* resource_;
    wl_resource* pending_buffer_ = nullptr;
    wl_resource* current_buffer_ = nullptr;
    bool pending_buffer_dirty_ = false;

public:
    explicit Surface(wl_resource* resource) noexcept : resource_(resource) {}
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    /// Fires on each wl_surface.commit, after pending state becomes current.
    Signal<SurfaceCommit> commit;

    /// True once a non-null buffer has been committed.
    [[nodiscard]] bool has_buffer() const noexcept { return current_buffer_ != nullptr; }

    /// Escape hatch for protocol wiring (e.g. seat focus): the wl_surface resource.
    [[nodiscard]] wl_resource* c_resource() const noexcept { return resource_; }

    /// Copy the committed wl_shm buffer into tightly-packed RGBA8 (`out`), setting
    /// width/height. Returns false if there is no buffer or it isn't a supported
    /// shm format (ARGB8888/XRGB8888). This is the bridge into the renderer.
    [[nodiscard]] bool current_buffer_rgba(std::vector<std::uint8_t>& out, int& width,
                                           int& height) const;

    // --- internal: called by the protocol glue in compositor.cpp ---
    void set_pending_buffer(wl_resource* buffer) noexcept {
        pending_buffer_ = buffer;
        pending_buffer_dirty_ = true;
    }
    void add_frame_callback(wl_resource* callback) { frame_callbacks_.push_back(callback); }
    void apply_commit();

private:
    std::vector<wl_resource*> frame_callbacks_; // pending wl_surface.frame callbacks
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
