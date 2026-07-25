module;

#include <string>

#include <wayland-server-core.h>

module luminaria;

namespace luminaria {

Result<Display> Display::create() {
    wl_display* d = wl_display_create();
    if (d == nullptr) {
        return fail("wl_display_create() failed");
    }
    return Display{d};
}

Display::~Display() {
    if (display_ != nullptr) {
        wl_display_destroy(display_);
    }
}

Display& Display::operator=(Display&& o) noexcept {
    if (this != &o) {
        if (display_ != nullptr) {
            wl_display_destroy(display_);
        }
        display_ = o.display_;
        o.display_ = nullptr;
    }
    return *this;
}

EventLoop Display::event_loop() const {
    return EventLoop{wl_display_get_event_loop(display_)};
}

Result<std::string> Display::add_socket_auto() {
    const char* name = wl_display_add_socket_auto(display_);
    if (name == nullptr) {
        return fail("wl_display_add_socket_auto() failed");
    }
    return std::string{name};
}

Status Display::init_shm() {
    if (wl_display_init_shm(display_) != 0) {
        return fail("wl_display_init_shm() failed");
    }
    return ok();
}

void Display::run() {
    wl_display_run(display_);
}

void Display::terminate() {
    wl_display_terminate(display_);
}

} // namespace luminaria
