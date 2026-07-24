// Phase 1 slice A: headless backend drives the output/commit/frame loop.
// Validates Backend->new_output, the software frame pump, commit, and event-loop
// integration end to end — no GPU. Solid color stands in for a rendered buffer.
#include <cassert>
#include <vector>

#include "luminaria/backend/headless.hpp"
#include "luminaria/core/display.hpp"

int main() {
    auto display = luminaria::Display::create();
    assert(display.has_value());

    luminaria::HeadlessBackend backend(display->event_loop(), /*frame_interval_ms=*/1);
    backend.add_output(800, 600);

    const luminaria::Color red{1.0f, 0.0f, 0.0f, 1.0f};
    int frames = 0;
    luminaria::Color got{};
    std::vector<luminaria::Signal<luminaria::FrameEvent>::Connection> frame_conns;

    auto out_conn = backend.new_output.connect([&](luminaria::NewOutput& e) {
        assert(e.output.width() == 800 && e.output.height() == 600);
        frame_conns.push_back(e.output.frame.connect([&](luminaria::FrameEvent& fe) {
            ++frames;
            (void)fe.output.commit(red);
            got = fe.output.last_committed();
            display->terminate(); // stop after first frame
        }));
    });

    (void)backend.start();
    display->run();

    assert(frames >= 1);
    assert(got == red);
    return 0;
}
