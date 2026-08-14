// luminaria/backend/libinput.cppm — bare-metal input via libinput. Emits keyboard and
// pointer events the compositor routes into a Seat.
//
// Devices are opened through a luminaria::Session (libseat) when one is given,
// and directly otherwise — in which case it relies on the logind ACLs a
// logged-in VT gets. Codes are raw evdev (what wl_keyboard/wl_pointer expect),
// so they pass straight to Seat.
//
// SAFETY: create() only builds the context; start() opens the devices (and thus
// grabs input). Never call start() under another compositor or you steal input.

module;

#include <cstdint>
#include <memory>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <map>
#include <string>
#include <libinput.h>
#include <libudev.h>

export module luminaria:libinput;

import :event_loop;
import :expected;
import :input_event;
import :session;
import :signal;

export namespace luminaria {

class Session;

class LibinputBackend {
public:
    /// Build the libinput/udev context. Does NOT open any input device yet.
    ///
    /// Pass a `Session` (luminaria/session.cppm) and devices are opened through
    /// libseat, so a VT switch revokes them cleanly and input is suspended
    /// while another session has the console. Without one it falls back to
    /// opening /dev/input/event* directly, which needs the logind ACLs of a
    /// logged-in VT and cannot survive a switch.
    [[nodiscard]] static Result<LibinputBackend> create(EventLoop loop, Session* session = nullptr);

    ~LibinputBackend();
    LibinputBackend(LibinputBackend&&) noexcept;
    LibinputBackend& operator=(LibinputBackend&&) noexcept;
    LibinputBackend(const LibinputBackend&) = delete;
    LibinputBackend& operator=(const LibinputBackend&) = delete;

    /// Assign the seat, open devices, and start delivering events. GRABS INPUT.
    Status start();

    [[nodiscard]] Signal<KeyEvent>& key() noexcept;
    [[nodiscard]] Signal<PointerMotionEvent>& pointer_motion() noexcept;
    [[nodiscard]] Signal<PointerButtonEvent>& pointer_button() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit LibinputBackend(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct LibinputBackend::Impl {
    EventLoop loop;
    udev* udev_ctx = nullptr;
    libinput* li = nullptr;
    EventSource fd_source;

    // With a session, every /dev/input/event* goes through libseat so the
    // kernel can revoke it on a VT switch. The map is what close_restricted
    // needs to give the device back by id rather than by fd.
    Session* session = nullptr;
    std::map<int, int> session_devices; // fd -> libseat device id
    Signal<SessionActive>::Connection session_conn;

    Signal<KeyEvent> key;
    Signal<PointerMotionEvent> pointer_motion;
    Signal<PointerButtonEvent> pointer_button;

    ~Impl() {
        fd_source = EventSource{};
        if (li != nullptr) {
            libinput_unref(li);
        }
        if (session != nullptr) {
            for (const auto& [fd, id] : session_devices) {
                session->close_device(id);
            }
            session_devices.clear();
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

int open_restricted(const char* path, int flags, void* data) {
    auto* impl = static_cast<LibinputBackend::Impl*>(data);
    if (impl != nullptr && impl->session != nullptr) {
        int id = -1;
        if (auto fd = impl->session->open_device(path, id)) {
            impl->session_devices[*fd] = id;
            return *fd;
        }
        return -EACCES;
    }
    int fd = open(path, flags | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}
void close_restricted(int fd, void* data) {
    auto* impl = static_cast<LibinputBackend::Impl*>(data);
    if (impl != nullptr && impl->session != nullptr) {
        if (auto it = impl->session_devices.find(fd); it != impl->session_devices.end()) {
            impl->session->close_device(it->second);
            impl->session_devices.erase(it);
            return; // the session owns the fd
        }
    }
    close(fd);
}
constexpr libinput_interface kInterface{open_restricted, close_restricted};

} // namespace

LibinputBackend::LibinputBackend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LibinputBackend::~LibinputBackend() = default;
LibinputBackend::LibinputBackend(LibinputBackend&&) noexcept = default;
LibinputBackend& LibinputBackend::operator=(LibinputBackend&&) noexcept = default;

Result<LibinputBackend> LibinputBackend::create(EventLoop loop, Session* session) {
    udev* udev_ctx = udev_new();
    if (udev_ctx == nullptr) {
        return fail("libinput: udev_new failed");
    }
    auto impl = std::make_unique<Impl>();
    impl->loop = loop;
    impl->udev_ctx = udev_ctx;
    impl->session = session;
    // The context's user data is the Impl, so open_restricted can reach the
    // session. It is created before the context for exactly that reason.
    libinput* li = libinput_udev_create_context(&kInterface, impl.get(), udev_ctx);
    if (li == nullptr) {
        udev_unref(udev_ctx);
        impl->udev_ctx = nullptr;
        return fail("libinput: create_context failed");
    }
    impl->li = li;
    return LibinputBackend{std::move(impl)};
}

Status LibinputBackend::start() {
    if (libinput_udev_assign_seat(impl_->li, "seat0") != 0) {
        return fail("libinput: assign_seat failed");
    }
    Impl* raw = impl_.get();
    impl_->fd_source = impl_->loop.add_fd(libinput_get_fd(impl_->li), [raw] { raw->dispatch(); });
    // libinput has to be told about the VT switch too, or it keeps reporting
    // keys pressed on someone else's session when we come back.
    if (impl_->session != nullptr) {
        impl_->session_conn =
            impl_->session->activity().connect([raw](SessionActive& event) {
                if (event.active) {
                    libinput_resume(raw->li);
                } else {
                    libinput_suspend(raw->li);
                }
            });
    }
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
