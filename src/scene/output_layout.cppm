// luminaria/output_layout.cppm — where the outputs sit relative to each other.
//
// A backend hands out `Output`s that each know only their own size. A desktop
// needs one shared coordinate space: which monitor is left of which, where a
// pointer at (2500, 400) actually is, which outputs a window straddles. That
// mapping lives here and nowhere else, so rendering, hit-testing and
// xdg-output all quote the same numbers.
//
// Pure geometry — no protocol, no rendering. Non-owning: an Output must be
// removed before it dies.

module;

#include <vector>

#include <algorithm>

export module luminaria:output_layout;

import :box;
import :output;

export namespace luminaria {

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

// --------------------------------------------------------------- implementation
namespace luminaria {

void OutputLayout::add(Output& output, int x, int y) {
    // Logical size, not the mode: a rotated or HiDPI output occupies a different
    // rectangle in the layout than its framebuffer suggests.
    const Box box{x, y, output.logical_width(), output.logical_height()};
    for (OutputBox& entry : outputs_) {
        if (entry.output == &output) {
            entry.box = box;
            return;
        }
    }
    outputs_.push_back(OutputBox{&output, box});
}

void OutputLayout::add_auto(Output& output) {
    // Right edge of everything placed so far — the layout a two-monitor desk
    // has by default, and a sane starting point for anything else.
    int right = 0;
    for (const OutputBox& entry : outputs_) {
        if (entry.output != &output) {
            right = std::max(right, entry.box.x + entry.box.width);
        }
    }
    add(output, right, 0);
}

void OutputLayout::remove(const Output& output) {
    std::erase_if(outputs_, [&](const OutputBox& entry) { return entry.output == &output; });
}

Box OutputLayout::box_of(const Output& output) const {
    for (const OutputBox& entry : outputs_) {
        if (entry.output == &output) {
            return entry.box;
        }
    }
    return Box{};
}

Box OutputLayout::bounds() const {
    Box all{};
    for (const OutputBox& entry : outputs_) {
        all = all.union_with(entry.box);
    }
    return all;
}

Output* OutputLayout::at(int x, int y) const {
    for (const OutputBox& entry : outputs_) {
        if (entry.box.contains(x, y)) {
            return entry.output;
        }
    }
    return nullptr;
}

std::vector<OutputBox> OutputLayout::intersecting(const Box& box) const {
    std::vector<OutputBox> hits;
    for (const OutputBox& entry : outputs_) {
        if (const Box overlap = entry.box.intersection(box); !overlap.empty()) {
            hits.push_back(OutputBox{entry.output, overlap});
        }
    }
    return hits;
}

} // namespace luminaria
