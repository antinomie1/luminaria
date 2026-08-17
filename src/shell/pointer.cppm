// luminaria/shell/pointer.cppm — cursor coordinates and pointer state tracking.
export module luminaria:pointer;

import std;

import :box;

export namespace luminaria {

/// The cursor's position, in logical coordinates.
class Pointer {
public:
    [[nodiscard]] bool present() const noexcept { return present_; }
    [[nodiscard]] double x() const noexcept { return x_; }
    [[nodiscard]] double y() const noexcept { return y_; }

    /// Put the pointer at an absolute position, clamped into `bounds`.
    /// Returns whether that moved it (arriving counts, repeated same position does not).
    bool move_to(double x, double y, const Box& bounds) noexcept;

    /// Move the pointer by a relative delta, clamped into `bounds`.
    /// Starts from centre of `bounds` if not already present.
    bool move_by(double dx, double dy, const Box& bounds) noexcept;

    /// The cursor left the output. Returns whether it had been present.
    bool leave() noexcept;

private:
    double x_ = 0.0;
    double y_ = 0.0;
    bool present_ = false;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

bool Pointer::move_to(double x, double y, const Box& bounds) noexcept {
    const auto last = [](int origin, int extent) {
        return std::max(static_cast<double>(origin),
                        std::nextafter(static_cast<double>(origin + extent), 0.0));
    };
    const double clamped_x = std::clamp(x, static_cast<double>(bounds.x),
                                        last(bounds.x, bounds.width));
    const double clamped_y = std::clamp(y, static_cast<double>(bounds.y),
                                        last(bounds.y, bounds.height));

    const bool moved = !present_ || clamped_x != x_ || clamped_y != y_;
    x_ = clamped_x;
    y_ = clamped_y;
    present_ = true;
    return moved;
}

bool Pointer::move_by(double dx, double dy, const Box& bounds) noexcept {
    const double base_x = present_ ? x_ : bounds.x + bounds.width / 2.0;
    const double base_y = present_ ? y_ : bounds.y + bounds.height / 2.0;
    return move_to(base_x + dx, base_y + dy, bounds);
}

bool Pointer::leave() noexcept {
    return std::exchange(present_, false);
}

} // namespace luminaria
