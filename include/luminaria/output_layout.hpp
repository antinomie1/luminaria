// luminaria/output_layout.hpp — where the outputs sit relative to each other.
//
// A backend hands out `Output`s that each know only their own size. A desktop
// needs one shared coordinate space: which monitor is left of which, where a
// pointer at (2500, 400) actually is, which outputs a window straddles. That
// mapping lives here and nowhere else, so rendering, hit-testing and
// xdg-output all quote the same numbers.
//
// Pure geometry — no protocol, no rendering. Non-owning: an Output must be
// removed before it dies.
#pragma once

#include <vector>

#include "luminaria/output.hpp"
#include "luminaria/util/box.hpp"

namespace luminaria {

/// An output and its rectangle in layout coordinates.
struct OutputBox {
    Output* output;
    Box box;
};

class OutputLayout {
public:
    /// Place `output` with its top-left at (x, y). Re-adding moves it.
    void add(Output& output, int x, int y);
    /// Place `output` immediately right of everything placed so far, tops aligned.
    void add_auto(Output& output);
    void remove(const Output& output);

    /// The output's rectangle, or an empty Box if it isn't in the layout.
    [[nodiscard]] Box box_of(const Output& output) const;
    /// Smallest rectangle covering every output. Empty when there are none.
    [[nodiscard]] Box bounds() const;
    /// The output containing layout point (x, y), or null in a gap.
    [[nodiscard]] Output* at(int x, int y) const;

    /// Outputs `box` (a window, in layout coordinates) touches, each paired with
    /// the part of `box` that lands on it — still in layout coordinates.
    /// Subtract `box_of(output)`'s origin to get output-local coordinates.
    [[nodiscard]] std::vector<OutputBox> intersecting(const Box& box) const;

    [[nodiscard]] const std::vector<OutputBox>& outputs() const noexcept { return outputs_; }
    [[nodiscard]] bool empty() const noexcept { return outputs_.empty(); }

private:
    std::vector<OutputBox> outputs_;
};

} // namespace luminaria
