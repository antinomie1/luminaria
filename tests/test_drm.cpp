#include <typeinfo>
// DRM backend: modeset a real monitor and page-flip frames. Skips (exit 77)
// unless run from a VT that can become DRM master (so it no-ops under a desktop
// session and actually runs on a bare tty).
#include <cassert>
#include <cstdio>

import luminaria;

int main() {
    auto display = luminaria::Display::create();
    assert(display.has_value());

    auto backend = luminaria::DrmBackend::create(display->event_loop());
    if (!backend) {
        std::fprintf(stderr, "skip: %s\n", backend.error().message.c_str());
        return 77;
    }

    bool saw_output = false;
    int frames = 0;
    luminaria::Signal<luminaria::FrameEvent>::Connection frame_conn;

    auto out_conn = backend->new_output.connect([&](luminaria::NewOutput& e) {
        saw_output = true;
        frame_conn = e.output.frame.connect([&](luminaria::FrameEvent& fe) {
            (void)fe.output.commit(luminaria::Color{0.2f, 0.3f, 0.6f, 1.0f});
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
