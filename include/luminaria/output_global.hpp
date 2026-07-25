// luminaria/output_global.hpp — the wl_output global.
//
// This is the protocol object clients read to learn there's a display and how
// big it is. Distinct from the backend `Output` (which owns scanout): many
// clients (e.g. weston-terminal) won't map a window until they see a wl_output.
//
// Public header stays C-header-free.
//
// It also carries the matching `zxdg_output_manager_v1` (xdg-output-unstable-v1):
// wl_output only describes the physical mode, so tools that arrange outputs in a
// layout — screenshot utilities above all — read the *logical* position and size
// from xdg-output instead. `grim` warns and produces a zero-sized image without it.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "luminaria/core/expected.hpp"
#include "luminaria/util/transform.hpp"

struct wl_resource;
struct wl_client;

namespace luminaria {

class Display;

/// A single wl_output advertising a fixed mode. Move-only; pointer-stable state
/// so the libwayland global can hold a pointer to it.
class OutputGlobal {
public:
    /// Advertise an output of `width`x`height` px at 60Hz, scale 1, under
    /// `name` (what `grim -o` and other tools address it by).
    [[nodiscard]] static Result<OutputGlobal> create(Display& display, int width, int height,
                                                     std::string name = "luminaria-1");

    ~OutputGlobal();
    OutputGlobal(OutputGlobal&&) noexcept;
    OutputGlobal& operator=(OutputGlobal&&) noexcept;
    OutputGlobal(const OutputGlobal&) = delete;
    OutputGlobal& operator=(const OutputGlobal&) = delete;

    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;

    /// Register a callback invoked for every wl_output resource created
    /// (past and future). Used by screencopy to track capturable outputs.
    using BindFunc = std::function<void(wl_resource*)>;
    void on_bind(BindFunc fn);

    /// This output's wl_output resource for `client`, or null if that client
    /// never bound it. Protocols that reference an output in an event
    /// (ext-workspace, presentation-time) can only name a client's own object.
    [[nodiscard]] wl_resource* resource_for(wl_client* client) const;

    /// Where this output sits in the layout, as reported by xdg-output. Set it
    /// from an OutputLayout so tools that arrange screenshots get real numbers.
    void set_logical_position(int x, int y);

    /// Integer output scale (wl_output.scale). A HiDPI client renders at this
    /// scale and the compositor no longer has to upscale a blurry buffer.
    void set_scale(int scale);
    /// Rotation/reflection of this output (wl_output.geometry transform).
    void set_transform(Transform transform);

    [[nodiscard]] int scale() const noexcept;
    [[nodiscard]] Transform transform() const noexcept;

    /// Size in layout coordinates: the mode divided by the scale, with the axes
    /// swapped when the transform rotates. This is what xdg-output reports.
    [[nodiscard]] int logical_width() const noexcept;
    [[nodiscard]] int logical_height() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit OutputGlobal(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
