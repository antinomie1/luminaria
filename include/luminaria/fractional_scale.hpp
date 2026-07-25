// luminaria/fractional_scale.hpp — wp_fractional_scale_v1.
//
// `wl_output.scale` is an integer, so a 150% display can only be told to clients
// as "1" (too small) or "2" (too big, then downscaled — blurry). This protocol
// says the real number, in 120ths: 180 is 1.5x.
//
// The client then renders at that density and uses wp_viewporter to declare the
// logical size of the buffer it produced, which is why the two travel together.
#pragma once

#include <memory>

#include "luminaria/core/expected.hpp"

namespace luminaria {

class Display;
class Surface;

class FractionalScaleManager {
public:
    [[nodiscard]] static Result<FractionalScaleManager> create(Display& display);

    ~FractionalScaleManager();
    FractionalScaleManager(FractionalScaleManager&&) noexcept;
    FractionalScaleManager& operator=(FractionalScaleManager&&) noexcept;
    FractionalScaleManager(const FractionalScaleManager&) = delete;
    FractionalScaleManager& operator=(const FractionalScaleManager&) = delete;

    /// Tell a surface the scale of the output it is showing on. `scale_120ths`
    /// is the protocol's unit (120 = 1x, 180 = 1.5x, 240 = 2x); values below 1
    /// are ignored. Call it when a surface lands on an output, and again when
    /// that output's scale changes. Surfaces with no wp_fractional_scale_v1
    /// object are silently skipped.
    void set_scale(Surface& surface, int scale_120ths);

    /// The same thing from a floating-point scale, e.g. 1.5.
    void set_scale(Surface& surface, double scale) {
        set_scale(surface, static_cast<int>(scale * 120.0 + 0.5));
    }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit FractionalScaleManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
