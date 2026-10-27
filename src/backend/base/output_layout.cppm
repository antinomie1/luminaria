// luminaria/shell/output_layout.cppm — where the outputs sit relative to each other.
//
// A backend hands out `Output`s that each know only their own size. A desktop
// needs one shared coordinate space: which monitor is left of which, where a
// pointer at (2500, 400) actually is, which outputs a window straddles. That
// mapping lives here and nowhere else, so rendering, hit-testing and
// xdg-output all quote the same numbers.
//
// Pure geometry — no protocol, no rendering. Non-owning, but it follows what it
// holds: an output that is destroyed leaves the layout by itself, and one whose
// mode changes keeps its position and takes its new size. Both used to be two
// handlers every compositor wrote, and forgetting the first left a dangling
// `Output*` in the one table every hit-test reads.

module;



export module luminaria:output_layout;

import std;

import :box;
import :output;
import :signal;

export namespace luminaria {

/// An output and its rectangle in layout coordinates.
struct OutputBox {
    Output* output;
    Box box;
};

/// Some output moved, arrived, left or changed size in the layout.
struct OutputLayoutChange {};

/// Not movable: each output it holds is watched on its behalf.
class OutputLayout {
public:
    OutputLayout() = default;
    OutputLayout(const OutputLayout&) = delete;
    OutputLayout& operator=(const OutputLayout&) = delete;

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

    /// The first output placed, the one a window with no better idea goes to;
    /// null when there are none.
    [[nodiscard]] Output* primary() const noexcept {
        return outputs_.empty() ? nullptr : outputs_.front().output;
    }
    /// Its rectangle; empty when there are none.
    [[nodiscard]] Box primary_box() const noexcept {
        return outputs_.empty() ? Box{} : outputs_.front().box;
    }

    [[nodiscard]] const std::vector<OutputBox>& outputs() const noexcept { return outputs_; }
    [[nodiscard]] bool empty() const noexcept { return outputs_.empty(); }

    /// Fires after every change to any output's box, including the ones the
    /// layout makes by itself (destroy, mode change).
    Signal<OutputLayoutChange> changed;

private:
    struct Watch {
        Output* output;
        Signal<OutputDestroy>::Connection on_destroy;
        Signal<OutputModeChange>::Connection on_mode;
    };
    std::vector<OutputBox> outputs_;
    std::vector<Watch> watches_;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

void OutputLayout::add(Output& output, int x, int y) {
    // Logical size, not the mode: a rotated or HiDPI output occupies a different
    // rectangle in the layout than its framebuffer suggests.
    const Box box{x, y, output.logical_width(), output.logical_height()};
    const auto known = std::ranges::find(outputs_, &output, &OutputBox::output);
    if (known != outputs_.end()) {
        known->box = box;
    } else {
        outputs_.push_back(OutputBox{&output, box});
        Watch& watch = watches_.emplace_back(Watch{&output, {}, {}});
        watch.on_destroy = output.destroy.connect([this](OutputDestroy& e) { remove(e.output); });
        watch.on_mode = output.mode_changed.connect([this](OutputModeChange& e) {
            const Box old = box_of(e.output);
            add(e.output, old.x, old.y);
        });
    }
    OutputLayoutChange change;
    changed.emit(change);
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
    if (std::erase_if(outputs_, [&](const OutputBox& entry) { return entry.output == &output; }) ==
        0) {
        return;
    }
    // Erasing the watch inside its own destroy slot is safe: a Signal never
    // frees the slot that is running.
    std::erase_if(watches_, [&](const Watch& watch) { return watch.output == &output; });
    OutputLayoutChange change;
    changed.emit(change);
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
