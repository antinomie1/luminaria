#include <typeinfo>
// luminaria-drm-demo — run from a bare VT (Ctrl+Alt+F3, log in, stop your desktop).
// Modesets the first connected monitor and fades its color via page-flips.
// Exits after ~3s (or set LUMINARIA_EXIT_MS).
#include <cmath>
#include <cstdio>
#include <cstdlib>

import luminaria.gpu;

int main(int argc, char** argv) {
    // Device: argv[1], else $LUMINARIA_DRM_DEVICE, else /dev/dri/card0.
    const char* env = std::getenv("LUMINARIA_DRM_DEVICE");
    const char* device = argc > 1 ? argv[1] : env; // null -> auto-detect

    auto display = luminaria::Display::create();
    if (!display) {
        return 1;
    }
    auto backend = device != nullptr ? luminaria::DrmBackend::create(display->event_loop(), device)
                                     : luminaria::DrmBackend::create(display->event_loop());
    if (!backend) {
        std::fprintf(stderr, "drm-demo: %s\n", backend.error().message.c_str());
        return 1;
    }

    int frame = 0;
    luminaria::Signal<luminaria::FrameEvent>::Connection conn;
    auto out = backend->new_output.connect([&](luminaria::NewOutput& e) {
        std::printf("drm-demo: output %dx%d\n", e.output.width(), e.output.height());
        conn = e.output.frame.connect([&](luminaria::FrameEvent& fe) {
            const float t = static_cast<float>(frame++) * 0.05f;
            (void)fe.output.commit(luminaria::Color{0.5f + 0.5f * std::sin(t),
                                               0.5f + 0.5f * std::sin(t + 2.0f),
                                               0.5f + 0.5f * std::sin(t + 4.0f), 1.0f});
        });
    });

    (void)backend->start();

    const char* ms = std::getenv("LUMINARIA_EXIT_MS");
    auto timer = display->event_loop().add_timer([&] { display->terminate(); });
    timer.arm(ms != nullptr ? static_cast<unsigned>(std::atoi(ms)) : 3000);

    display->run();
    return 0;
}
