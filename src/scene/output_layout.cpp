#include "luminaria/output_layout.hpp"

#include <algorithm>

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
