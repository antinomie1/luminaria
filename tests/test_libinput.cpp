// libinput backend: verify the context builds. Deliberately does NOT call
// start() — start() opens input devices and would steal input from a running
// desktop. Real event delivery is exercised by the tty compositor example.
#include <cassert>
#include <cstdio>

import luminaria.gpu;
import std;

int main() {
    auto display = luminaria::Display::create();
    assert(display.has_value());

    auto backend = luminaria::LibinputBackend::create(display->event_loop());
    if (!backend) {
        std::fprintf(stderr, "skip: %s\n", backend.error().message.c_str());
        return 77;
    }
    // Context built without opening devices. start() is left to bare-metal use.
    return 0;
}
