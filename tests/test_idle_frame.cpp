// An output that nobody asks anything of must cost nothing.
//
// This is the backend half of the low-power step: `frame` is not a free-running
// pump any more. A backend emits exactly one frame per `schedule_frame()`, and
// committing to an output does NOT implicitly ask for another one — otherwise a
// still screen keeps the CPU (and, on real hardware, the display engine) awake
// sixty times a second for a picture nobody changed.
//
// Headless is the backend that can assert it without hardware; DRM's frames
// come from page-flip completions and the nested backend's from the parent's
// callbacks, so both stop on their own once the commits stop.
#include <cassert>
#include <vector>

import luminaria;

int main() {
    auto display = luminaria::Display::create();
    assert(display.has_value());

    // 1ms pacing: any free-running pump would rack up dozens of frames inside
    // the windows below, so "exactly one" is not an accident of timing.
    luminaria::HeadlessBackend backend(display->event_loop(), /*frame_interval_ms=*/1);
    luminaria::Output& output = backend.add_output(64, 48);

    int frames = 0;
    int commits = 0;
    auto conn = output.frame.connect([&](luminaria::FrameEvent& e) {
        ++frames;
        // A compositor that draws every frame it is given, and asks for nothing.
        (void)e.output.commit(luminaria::Color{0, 0, 0, 1});
        ++commits;
    });

    assert(!output.frame_scheduled());
    (void)backend.start(); // the first frame is asked for on the output's behalf
    assert(output.frame_scheduled());

    luminaria::EventSource wake;
    luminaria::EventSource stop;

    wake = display->event_loop().add_timer([&] {
        // 50ms of a 1ms pump: one frame, because only one was ever asked for.
        assert(frames == 1);
        assert(commits == 1);
        assert(!output.frame_scheduled());

        output.schedule_frame();
        assert(output.frame_scheduled());
        // Asking twice does not queue two frames.
        output.schedule_frame();
    });
    wake.arm(50);

    stop = display->event_loop().add_timer([&] {
        assert(frames == 2);
        assert(!output.frame_scheduled());
        display->terminate();
    });
    stop.arm(100);

    display->run();
    assert(frames == 2);
    return 0;
}
