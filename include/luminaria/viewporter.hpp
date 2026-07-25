// luminaria/viewporter.hpp — wp_viewporter: crop and scale a surface's buffer.
//
// A client attaches one buffer and says "show this sub-rectangle of it, at this
// size". Video players use it to letterbox without re-encoding; every client
// doing fractional scaling uses it to render at, say, 1.5x into an integer
// buffer and have the compositor stretch it to the right logical size.
//
// The state lands on the Surface (`viewport_src_*`, `surface_width/height`); the
// renderer reads it from there, so nothing else has to know this global exists.
#pragma once

#include <memory>

#include "luminaria/core/expected.hpp"

namespace luminaria {

class Display;

class Viewporter {
public:
    [[nodiscard]] static Result<Viewporter> create(Display& display);

    ~Viewporter();
    Viewporter(Viewporter&&) noexcept;
    Viewporter& operator=(Viewporter&&) noexcept;
    Viewporter(const Viewporter&) = delete;
    Viewporter& operator=(const Viewporter&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit Viewporter(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
