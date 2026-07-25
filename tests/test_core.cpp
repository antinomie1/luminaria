#include <typeinfo>
#include <utility>
// Phase 0 acceptance: a Display + EventLoop can be created, an idle callback
// runs, and run() returns after terminate(). Exercises RAII create/destroy too.
#include <cassert>

import luminaria;

int main() {
    auto display = luminaria::Display::create();
    assert(display.has_value());

    auto loop = display->event_loop();
    assert(loop.valid());

    bool ran = false;
    loop.once([&] {
        ran = true;
        display->terminate();
    });

    display->run(); // must return once the idle fires terminate()
    assert(ran);

    // Move-assignment must not double-free.
    auto other = luminaria::Display::create();
    assert(other.has_value());
    *display = std::move(*other);

    return 0;
}
