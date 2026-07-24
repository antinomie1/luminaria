// luminaria/output_global.hpp — the wl_output global.
//
// This is the protocol object clients read to learn there's a display and how
// big it is. Distinct from the backend `Output` (which owns scanout): many
// clients (e.g. weston-terminal) won't map a window until they see a wl_output.
//
// Public header stays C-header-free.
#pragma once

#include <functional>
#include <memory>

#include "luminaria/core/expected.hpp"

struct wl_resource;

namespace luminaria {

class Display;

/// A single wl_output advertising a fixed mode. Move-only; pointer-stable state
/// so the libwayland global can hold a pointer to it.
class OutputGlobal {
public:
    /// Advertise an output of `width`x`height` px at 60Hz, scale 1.
    [[nodiscard]] static Result<OutputGlobal> create(Display& display, int width, int height);

    ~OutputGlobal();
    OutputGlobal(OutputGlobal&&) noexcept;
    OutputGlobal& operator=(OutputGlobal&&) noexcept;
    OutputGlobal(const OutputGlobal&) = delete;
    OutputGlobal& operator=(const OutputGlobal&) = delete;

    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;

    /// Register a callback invoked for every wl_output resource created
    /// (past and future). Used by screencopy to track capturable outputs.
    using BindFunc = std::function<void(wl_resource*)>;
    void on_bind(BindFunc fn);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit OutputGlobal(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
