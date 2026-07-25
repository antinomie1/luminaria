#include <typeinfo>
// Nested backend against a real parent compositor: connect, open a window, and
// present frames. Skips (exit 77) when no parent compositor is available.
#include <cassert>
#include <cstdio>
#include <cstdlib>

import luminaria;

int main() {
    if (std::getenv("WAYLAND_DISPLAY") == nullptr) {
        std::fprintf(stderr, "skip: no WAYLAND_DISPLAY (no parent compositor)\n");
        return 77;
    }

    auto display = luminaria::Display::create();
    assert(display.has_value());

    auto backend = luminaria::WaylandBackend::create(display->event_loop());
    if (!backend) {
        std::fprintf(stderr, "skip: %s\n", backend.error().message.c_str());
        return 77;
    }

    backend->add_output(400, 300);

    bool saw_output = false;
    int frames = 0;
    luminaria::Signal<luminaria::FrameEvent>::Connection frame_conn;

    auto out_conn = backend->new_output.connect([&](luminaria::NewOutput& e) {
        saw_output = true;
        assert(e.output.width() == 400 && e.output.height() == 300);
        frame_conn = e.output.frame.connect([&](luminaria::FrameEvent& fe) {
            (void)fe.output.commit(luminaria::Color{0.2f, 0.4f, 0.8f, 1.0f});
            if (++frames >= 3) {
                display->terminate();
            }
        });
    });

    assert(backend->start().has_value());

    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(3000);

    display->run();

    assert(saw_output);
    assert(frames >= 1);
    return 0;
}
