// What follows an output by itself: the layout (an output that is destroyed
// leaves it, one whose mode changes keeps its place and takes its new size,
// and every change is announced) and a wl_output created from the output and
// the layout (it re-reads the mode and the position). Both used to be
// handlers every compositor wrote; forgetting the first left a dangling
// `Output*` in the table every hit-test reads.
#include <cassert>

import luminaria;
import std;

namespace {

/// An output whose mode the test changes, the way DRM's `set_mode` does.
class TestOutput final : public luminaria::Output {
public:
    TestOutput(int width, int height) : Output(width, height) {}
    luminaria::Status commit(luminaria::Color) override { return luminaria::ok(); }
    void switch_mode(int width, int height) {
        width_ = width;
        height_ = height;
        luminaria::OutputModeChange event{*this, current_mode()};
        mode_changed.emit(event);
    }
};

} // namespace

int main() {
    auto display = luminaria::Display::create();
    assert(display.has_value());

    luminaria::OutputLayout layout;
    int changes = 0;
    auto counted = layout.changed.connect([&](luminaria::OutputLayoutChange&) { ++changes; });

    auto left = std::make_unique<TestOutput>(800, 600);
    TestOutput right(1280, 720);
    layout.add_auto(*left);
    layout.add_auto(right);
    assert(changes == 2);
    assert((layout.box_of(right) == luminaria::Box{800, 0, 1280, 720}));

    auto global = luminaria::OutputGlobal::create(*display, right, layout);
    assert(global.has_value());
    assert(global->width() == 1280 && global->height() == 720);
    assert(global->name().starts_with("luminaria-"));
    auto second = luminaria::OutputGlobal::create(*display, *left, layout);
    assert(second.has_value() && second->name() != global->name());

    // A mode change: same place, new size, and the wl_output says so.
    right.switch_mode(1920, 1080);
    assert((layout.box_of(right) == luminaria::Box{800, 0, 1920, 1080}));
    assert(changes == 3);
    assert(global->width() == 1920 && global->height() == 1080);

    // Destroyed: out of the layout without anyone calling remove().
    left.reset();
    assert(layout.outputs().size() == 1);
    assert(layout.at(10, 10) == nullptr);
    assert(changes == 4);
    // The global that described it stopped following and is still safe.
    assert(second->width() == 800);

    // Explicit removal still works, announces itself once, and is idempotent.
    layout.remove(right);
    layout.remove(right);
    assert(layout.empty());
    assert(changes == 5);
    // A mode change on an output the layout let go of changes nothing.
    right.switch_mode(640, 480);
    assert(layout.empty() && changes == 5);
    return 0;
}
