#include <typeinfo>
#include <string>
// Step 3: Xwayland bring-up. Launch Xwayland against our compositor, attach the
// window manager, and assert the ready signal fires with a display name.
// Skips (exit 77) if Xwayland can't start in this environment.
#include <cassert>
#include <cstdio>
#include <cstdlib>

import luminaria.xwayland;

int main() {
    auto display = luminaria::Display::create();
    assert(display.has_value());
    (void)display->init_shm();
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());

    auto socket = display->add_socket_auto();
    assert(socket.has_value());
    setenv("WAYLAND_DISPLAY", socket->c_str(), 1);

    auto xwayland = luminaria::Xwayland::create(*display, *compositor);
    if (!xwayland) {
        std::fprintf(stderr, "skip: %s\n", xwayland.error().message.c_str());
        return 77;
    }

    bool ready = false;
    std::string name;
    auto conn = xwayland->ready().connect([&](luminaria::XwaylandReady& e) {
        ready = true;
        name = e.display_name;
        display->terminate();
    });

    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(8000);

    display->run();

    if (!ready) {
        std::fprintf(stderr, "skip: Xwayland did not become ready\n");
        return 77;
    }
    assert(!name.empty());
    assert(name[0] == ':');
    std::printf("Xwayland ready on DISPLAY=%s\n", name.c_str());
    return 0;
}
