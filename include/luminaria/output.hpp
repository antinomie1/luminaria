// luminaria/output.hpp — a display output the compositor renders to.
//
// Backend-specific behaviour (headless, nested, DRM) lives in subclasses; the
// compositor only sees this interface. `frame` fires when it's time to draw the
// next frame; the compositor responds by committing new content.
#pragma once

#include <span>

#include "luminaria/core/expected.hpp"
#include "luminaria/core/signal.hpp"
#include "luminaria/util/color.hpp"
#include "luminaria/util/pixel.hpp"

namespace luminaria {

class Output;

/// "Time to draw a new frame on `output`."
struct FrameEvent {
    Output& output;
};

class Output {
public:
    virtual ~Output() = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

    Signal<FrameEvent> frame;

    /// Present a solid color (convenience / bring-up path).
    virtual Status commit(Color color) = 0;

    /// Present a rendered RGBA frame (row-major, width*height pixels). This is the
    /// real compositing path: render the scene, then hand the pixels to scanout.
    /// Default: unsupported (backends that can scan out pixels override it).
    virtual Status commit_frame(std::span<const Pixel> rgba, int width, int height) {
        (void)rgba;
        (void)width;
        (void)height;
        return fail("this output cannot present a pixel frame");
    }

    [[nodiscard]] Color last_committed() const noexcept { return last_committed_; }

protected:
    Output(int width, int height) noexcept : width_(width), height_(height) {}

    int width_;
    int height_;
    Color last_committed_{};
};

} // namespace luminaria
