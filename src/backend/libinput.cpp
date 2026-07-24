#include "luminaria/backend/libinput.hpp"

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

#include <libinput.h>
#include <libudev.h>

namespace luminaria {

struct LibinputBackend::Impl {
    EventLoop loop;
    udev* udev_ctx = nullptr;
    libinput* li = nullptr;
    EventSource fd_source;

    Signal<KeyEvent> key;
    Signal<PointerMotionEvent> pointer_motion;
    Signal<PointerButtonEvent> pointer_button;

    ~Impl() {
        fd_source = EventSource{};
        if (li != nullptr) {
            libinput_unref(li);
        }
        if (udev_ctx != nullptr) {
            udev_unref(udev_ctx);
        }
    }

    void dispatch() {
        libinput_dispatch(li);
        libinput_event* event = nullptr;
        while ((event = libinput_get_event(li)) != nullptr) {
            switch (libinput_event_get_type(event)) {
            case LIBINPUT_EVENT_KEYBOARD_KEY: {
                auto* k = libinput_event_get_keyboard_event(event);
                KeyEvent e{libinput_event_keyboard_get_key(k),
                           libinput_event_keyboard_get_key_state(k) ==
                               LIBINPUT_KEY_STATE_PRESSED};
                key.emit(e);
                break;
            }
            case LIBINPUT_EVENT_POINTER_MOTION: {
                auto* p = libinput_event_get_pointer_event(event);
                PointerMotionEvent e{libinput_event_pointer_get_dx(p),
                                     libinput_event_pointer_get_dy(p)};
                pointer_motion.emit(e);
                break;
            }
            case LIBINPUT_EVENT_POINTER_BUTTON: {
                auto* p = libinput_event_get_pointer_event(event);
                PointerButtonEvent e{libinput_event_pointer_get_button(p),
                                     libinput_event_pointer_get_button_state(p) ==
                                         LIBINPUT_BUTTON_STATE_PRESSED};
                pointer_button.emit(e);
                break;
            }
            default:
                break;
            }
            libinput_event_destroy(event);
        }
    }
};

namespace {

int open_restricted(const char* path, int flags, void*) {
    int fd = open(path, flags | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}
void close_restricted(int fd, void*) {
    close(fd);
}
constexpr libinput_interface kInterface{open_restricted, close_restricted};

} // namespace

LibinputBackend::LibinputBackend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LibinputBackend::~LibinputBackend() = default;
LibinputBackend::LibinputBackend(LibinputBackend&&) noexcept = default;
LibinputBackend& LibinputBackend::operator=(LibinputBackend&&) noexcept = default;

Result<LibinputBackend> LibinputBackend::create(EventLoop loop) {
    udev* udev_ctx = udev_new();
    if (udev_ctx == nullptr) {
        return fail("libinput: udev_new failed");
    }
    libinput* li = libinput_udev_create_context(&kInterface, nullptr, udev_ctx);
    if (li == nullptr) {
        udev_unref(udev_ctx);
        return fail("libinput: create_context failed");
    }
    auto impl = std::make_unique<Impl>();
    impl->loop = loop;
    impl->udev_ctx = udev_ctx;
    impl->li = li;
    return LibinputBackend{std::move(impl)};
}

Status LibinputBackend::start() {
    if (libinput_udev_assign_seat(impl_->li, "seat0") != 0) {
        return fail("libinput: assign_seat failed");
    }
    Impl* raw = impl_.get();
    impl_->fd_source = impl_->loop.add_fd(libinput_get_fd(impl_->li), [raw] { raw->dispatch(); });
    return ok();
}

Signal<KeyEvent>& LibinputBackend::key() noexcept { return impl_->key; }
Signal<PointerMotionEvent>& LibinputBackend::pointer_motion() noexcept {
    return impl_->pointer_motion;
}
Signal<PointerButtonEvent>& LibinputBackend::pointer_button() noexcept {
    return impl_->pointer_button;
}

} // namespace luminaria
