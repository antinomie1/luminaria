// luminaria/subcompositor.cppm — the wl_subcompositor global (subsurfaces).
//
// A subsurface is a wl_surface positioned relative to a parent wl_surface and
// stacked below or above it. Toolkits use them for video layers, client-side
// decorations, and popups drawn inside a window. The tree itself lives on
// `Surface` (see compositor.cppm: subsurface_parent / surface_tree /
// surface_at); this global is just the protocol glue that builds it.
//
// Sync mode (the protocol default) is implemented: a synced subsurface's commit
// is cached and applied atomically when its parent commits.

module;

#include <memory>

export module luminaria:subcompositor;

import :core.expected;

export namespace luminaria {

class Display;

/// The wl_subcompositor global (protocol version 1). Move-only; pointer-stable
/// state so the libwayland global can hold a pointer to it.
class Subcompositor {
public:
    [[nodiscard]] static Result<Subcompositor> create(Display& display);

    ~Subcompositor();
    Subcompositor(Subcompositor&&) noexcept;
    Subcompositor& operator=(Subcompositor&&) noexcept;
    Subcompositor(const Subcompositor&) = delete;
    Subcompositor& operator=(const Subcompositor&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit Subcompositor(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
