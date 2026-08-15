#include <typeinfo>
// DRM backend: modeset a real monitor and page-flip frames. Skips (exit 77)
// unless run from a VT that can become DRM master (so it no-ops under a desktop
// session and actually runs on a bare tty).
//
// When the monitor offers a second mode, it is switched to and back: a modeset
// is the one operation that invalidates every framebuffer at once, so the
// interesting part is that the output keeps flipping afterwards.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

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
    luminaria::Output* first = nullptr;
    luminaria::Signal<luminaria::FrameEvent>::Connection frame_conn;
    luminaria::Signal<luminaria::OutputModeChange>::Connection mode_conn;
    std::vector<luminaria::OutputMode> announced;

    auto out_conn = backend->new_output.connect([&](luminaria::NewOutput& e) {
        saw_output = true;
        if (first == nullptr) {
            first = &e.output;
            mode_conn = e.output.mode_changed.connect(
                [&](luminaria::OutputModeChange& me) { announced.push_back(me.mode); });
        }
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

    // A real connector always reports at least the mode it is running, and the
    // one it is running has to be in the list.
    assert(first != nullptr);
    const std::vector<luminaria::OutputMode> modes = first->modes();
    assert(!modes.empty());
    const luminaria::OutputMode started_at = first->current_mode();
    assert(std::find(modes.begin(), modes.end(), started_at) != modes.end());
    assert(started_at.width == first->width() && started_at.height == first->height());
    // Re-asking for the current mode changes nothing and announces nothing.
    assert(first->set_mode(started_at.width, started_at.height, started_at.refresh_mhz));
    assert(announced.empty());
    // A size that no connector offers is refused, not half-applied.
    assert(!first->set_mode(3, 5));
    assert(first->width() == started_at.width);

    // Switch to a genuinely different size if the monitor has one, then back,
    // and check frames still arrive on the other side of both modesets.
    const luminaria::OutputMode* other = nullptr;
    for (const luminaria::OutputMode& m : modes) {
        if (m.width != started_at.width || m.height != started_at.height) {
            other = &m;
            break;
        }
    }
    if (other == nullptr) {
        std::fprintf(stderr, "note: connector offers only one size; mode switch not exercised\n");
        return 0;
    }

    const int want_w = other->width, want_h = other->height;
    if (auto s = first->set_mode(want_w, want_h); !s) {
        std::fprintf(stderr, "skip: set_mode: %s\n", s.error().message.c_str());
        return 77;
    }
    assert(first->width() == want_w && first->height() == want_h);
    assert(announced.size() == 1 && announced[0].width == want_w);

    frames = 0;
    auto timeout2 = display->event_loop().add_timer([&] { display->terminate(); });
    timeout2.arm(3000);
    display->run();
    assert(frames >= 1); // the flip chain survived the modeset

    assert(first->set_mode(started_at.width, started_at.height, started_at.refresh_mhz));
    assert(first->width() == started_at.width && first->height() == started_at.height);
    assert(announced.size() == 2);
    return 0;
}
