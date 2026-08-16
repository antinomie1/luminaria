// luminaria/xwayland.cppm — run X11 apps by launching Xwayland and managing its
// windows. Spawns the Xwayland server (rootless), connects a minimal X window
// manager over xcb, and redirects the root so X windows can be mapped.
//
// TODO: minimal XWM — map/configure requests handled; full ICCCM/EWMH,
// override-redirect, and wl_surface association land as real X clients need them.

module;


#include <cerrno>
#include <csignal>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb.h>

export module luminaria.xwayland;

import std;

export import luminaria;

export namespace luminaria {

struct XwaylandReady {
    std::string display_name; // e.g. ":1"
};

class Xwayland {
public:
    /// Launch Xwayland connected to this compositor. The parent compositor must
    /// already have a socket (WAYLAND_DISPLAY) that Xwayland can connect to.
    [[nodiscard]] static Result<Xwayland> create(Display& display, Compositor& compositor);

    ~Xwayland();
    Xwayland(Xwayland&&) noexcept;
    Xwayland& operator=(Xwayland&&) noexcept;
    Xwayland(const Xwayland&) = delete;
    Xwayland& operator=(const Xwayland&) = delete;

    /// Fires once the X server is up and the window manager has attached.
    [[nodiscard]] Signal<XwaylandReady>& ready() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit Xwayland(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct Xwayland::Impl {
    EventLoop loop;
    pid_t pid = -1;

    int wm_fd = -1;         // our end of the WM X connection
    int display_read_fd = -1; // Xwayland writes its display number here
    EventSource display_source;
    EventSource xcb_source;

    xcb_connection_t* xcb = nullptr;
    xcb_window_t root = 0;
    std::string display_name;
    bool started = false;

    Signal<XwaylandReady> ready;

    ~Impl() {
        display_source = EventSource{};
        xcb_source = EventSource{};
        if (xcb != nullptr) {
            xcb_disconnect(xcb);
        }
        if (display_read_fd >= 0) {
            close(display_read_fd);
        }
        if (wm_fd >= 0) {
            close(wm_fd);
        }
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
    }
};

namespace {

void unset_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) {
        fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
    }
}

// Minimal window manager event pump: map whatever asks to be mapped, and honor
// configure requests as-is. Enough for X clients to show a window.
void handle_xcb_events(Xwayland::Impl* xwm) {
    using EventPtr = std::unique_ptr<xcb_generic_event_t, decltype(&std::free)>;
    while (EventPtr event{xcb_poll_for_event(xwm->xcb), std::free}) {
        switch (event->response_type & 0x7f) {
        case XCB_MAP_REQUEST: {
            auto* e = reinterpret_cast<xcb_map_request_event_t*>(event.get());
            xcb_map_window(xwm->xcb, e->window);
            break;
        }
        case XCB_CONFIGURE_REQUEST: {
            auto* e = reinterpret_cast<xcb_configure_request_event_t*>(event.get());
            const uint32_t values[] = {static_cast<uint32_t>(e->x), static_cast<uint32_t>(e->y),
                                       e->width, e->height, e->border_width, e->sibling,
                                       e->stack_mode};
            xcb_configure_window(xwm->xcb, e->window, e->value_mask, values);
            break;
        }
        default:
            break;
        }
    }
    xcb_flush(xwm->xcb);
}

// Attach the window manager once the X server is listening.
bool start_wm(Xwayland::Impl* xwm) {
    xwm->xcb = xcb_connect_to_fd(xwm->wm_fd, nullptr);
    xwm->wm_fd = -1; // owned by xcb now
    if (xcb_connection_has_error(xwm->xcb) != 0) {
        return false;
    }
    const xcb_setup_t* setup = xcb_get_setup(xwm->xcb);
    xcb_screen_t* screen = xcb_setup_roots_iterator(setup).data;
    xwm->root = screen->root;

    // Become the window manager: ask for substructure redirect on the root.
    const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                          XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_change_window_attributes(xwm->xcb, xwm->root, XCB_CW_EVENT_MASK, &mask);
    xcb_flush(xwm->xcb);

    xwm->xcb_source = xwm->loop.add_fd(xcb_get_file_descriptor(xwm->xcb),
                                       [xwm] { handle_xcb_events(xwm); });
    return true;
}

// Xwayland reports its display number on the display fd once it is ready.
void on_display_ready(Xwayland::Impl* xwm) {
    char buffer[32] = {0};
    const ssize_t n = read(xwm->display_read_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        return;
    }
    // Payload is the number then a newline, e.g. "1\n".
    int display_number = 0;
    std::from_chars(buffer, buffer + n, display_number);
    xwm->display_name = ":" + std::to_string(display_number);
    setenv("DISPLAY", xwm->display_name.c_str(), 1);

    xwm->display_source = EventSource{}; // one-shot; stop watching
    close(xwm->display_read_fd);
    xwm->display_read_fd = -1;

    if (!xwm->started && start_wm(xwm)) {
        xwm->started = true;
        XwaylandReady event{xwm->display_name};
        xwm->ready.emit(event);
    }
}

} // namespace

Xwayland::Xwayland(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Xwayland::~Xwayland() = default;
Xwayland::Xwayland(Xwayland&&) noexcept = default;
Xwayland& Xwayland::operator=(Xwayland&&) noexcept = default;

Result<Xwayland> Xwayland::create(Display& display, Compositor& /*compositor*/) {
    // Socketpair carrying the WM's X11 connection; pipe carrying the display number.
    int wm_socket[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, wm_socket) != 0) {
        return fail("xwayland: socketpair failed");
    }
    int display_pipe[2];
    if (pipe(display_pipe) != 0) {
        close(wm_socket[0]);
        close(wm_socket[1]);
        return fail("xwayland: pipe failed");
    }

    pid_t pid = fork();
    if (pid < 0) {
        return fail("xwayland: fork failed");
    }
    if (pid == 0) {
        // Child: hand the server its ends of the fds (clear CLOEXEC) and exec.
        unset_cloexec(wm_socket[1]);
        unset_cloexec(display_pipe[1]);
        std::string wm_arg = std::to_string(wm_socket[1]);
        std::string displayfd_arg = std::to_string(display_pipe[1]);
        execlp("Xwayland", "Xwayland", "-rootless", "-terminate", "-wm", wm_arg.c_str(),
               "-displayfd", displayfd_arg.c_str(), static_cast<char*>(nullptr));
        std::println(std::cerr, "xwayland: execlp failed: {}", std::generic_category().message(errno));
        _exit(127);
    }

    // Parent: keep our ends, close the child's.
    close(wm_socket[1]);
    close(display_pipe[1]);

    auto impl = std::make_unique<Impl>();
    impl->loop = display.event_loop();
    impl->pid = pid;
    impl->wm_fd = wm_socket[0];
    impl->display_read_fd = display_pipe[0];

    Impl* raw = impl.get();
    impl->display_source = impl->loop.add_fd(display_pipe[0], [raw] { on_display_ready(raw); });

    return Xwayland{std::move(impl)};
}

Signal<XwaylandReady>& Xwayland::ready() noexcept {
    return impl_->ready;
}

} // namespace luminaria
