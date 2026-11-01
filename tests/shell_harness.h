// What the tests of the shell's managed layer share: a backend whose frames only
// happen when the test says so, and a client on the same thread, so a test can
// do one thing, pump, and assert on what the other side saw.
//
// Include it after `import luminaria.shell;` and after the C headers it uses
// (wayland-client, wayland-server-core, xdg-shell-client-protocol, sys/mman,
// sys/socket, poll, unistd).
#pragma once

namespace harness {

namespace lm = luminaria;

/// An output paced the way a display is: `present` only for a frame that was
/// actually committed, and a `frame` only when one was asked for.
class ManualOutput final : public lm::Output {
public:
    ManualOutput(int width, int height) : Output(width, height) {}

    lm::Status commit(lm::Color) override { return flip(); }
    lm::Status commit_frame(std::span<const lm::Pixel>, int, int) override { return flip(); }

    /// What a page flip does. Answers whether a `frame` was delivered.
    bool fire() {
        if (std::exchange(flip_pending_, false)) {
            lm::PresentEvent shown{*this};
            const auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
            shown.tv_sec = static_cast<std::uint64_t>(ns / 1'000'000'000);
            shown.tv_nsec = static_cast<std::uint32_t>(ns % 1'000'000'000);
            present.emit(shown);
        }
        if (!std::exchange(asked_, false)) {
            return false;
        }
        emit_frame();
        return true;
    }

    int flips = 0;

protected:
    void arm_frame() override { asked_ = true; }

private:
    lm::Status flip() {
        flip_pending_ = true;
        ++flips;
        return lm::ok();
    }
    bool flip_pending_ = false;
    bool asked_ = false;
};

class ManualBackend final : public lm::Backend {
public:
    ManualOutput& add(int width, int height) {
        ManualOutput& output =
            *outputs_.emplace_back(std::make_unique<ManualOutput>(width, height));
        if (started_) {
            lm::NewOutput event{output};
            new_output.emit(event);
        }
        return output;
    }
    /// A monitor unplugged.
    void remove(ManualOutput& output) {
        std::erase_if(outputs_, [&](const auto& held) { return held.get() == &output; });
    }
    lm::Status start() override {
        started_ = true;
        for (const auto& output : outputs_) {
            lm::NewOutput event{*output};
            new_output.emit(event);
        }
        return lm::ok();
    }

private:
    std::vector<std::unique_ptr<ManualOutput>> outputs_;
    bool started_ = false;
};

/// A client on the test's own thread. `pump()` moves everything either side has
/// to say until both are quiet.
struct Client {
    lm::Display* server = nullptr;
    wl_client* server_client = nullptr;
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;

    std::vector<std::uint32_t> output_globals; // registry names still announced
    std::vector<std::string> output_names;     // wl_output.name, in arrival order

    int pointer_enters = 0, pointer_leaves = 0;
    double pointer_x = -1.0, pointer_y = -1.0;
    wl_surface* pointer_surface = nullptr;
    wl_surface* keyboard_surface = nullptr;
    std::vector<std::uint32_t> buttons; // pressed ones, then 0x10000 | released ones
    std::vector<std::uint32_t> keys;    // the same
    double axis = 0.0;
    int popups_done = 0;
    int popup_y = -1;
    int frames_done = 0;

    explicit Client(lm::Display& on) : server(&on) {
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        server_client = wl_client_create(on.c_ptr(), fds[0]);
        assert(server_client != nullptr);
        display = wl_display_connect_to_fd(fds[1]);
        assert(display != nullptr);
        wl_registry* registry = wl_display_get_registry(display);
        static const wl_registry_listener listener{on_global, on_global_remove};
        wl_registry_add_listener(registry, &listener, this);
        pump();
        assert(compositor != nullptr && shm != nullptr && wm_base != nullptr && seat != nullptr);
        static const xdg_wm_base_listener wm{[](void*, xdg_wm_base* base, std::uint32_t serial) {
            xdg_wm_base_pong(base, serial);
        }};
        xdg_wm_base_add_listener(wm_base, &wm, this);
        pump();
    }
    ~Client() {
        pump();
        wl_display_disconnect(display);
    }

    void pump() {
        wl_event_loop* loop = wl_display_get_event_loop(server->c_ptr());
        for (int quiet = 0; quiet < 4;) {
            const int before = wl_display_dispatch_pending(display);
            wl_display_flush(display);
            wl_event_loop_dispatch(loop, 0);
            wl_display_flush_clients(server->c_ptr());
            int read = 0;
            if (wl_display_prepare_read(display) == 0) {
                pollfd fd{wl_display_get_fd(display), POLLIN, 0};
                if (poll(&fd, 1, 0) > 0) {
                    read = 1;
                    wl_display_read_events(display);
                } else {
                    wl_display_cancel_read(display);
                }
            }
            quiet = before == 0 && read == 0 ? quiet + 1 : 0;
        }
    }

    wl_buffer* buffer(int width, int height, std::uint32_t pixel = 0xff808080u) {
        const int stride = width * 4;
        const int size = stride * height;
        const int fd = memfd_create("harness", MFD_CLOEXEC);
        assert(fd >= 0 && ftruncate(fd, size) == 0);
        void* map = mmap(nullptr, static_cast<std::size_t>(size), PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        assert(map != MAP_FAILED);
        std::fill_n(static_cast<std::uint32_t*>(map), width * height, pixel);
        munmap(map, static_cast<std::size_t>(size));
        wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
        wl_buffer* out = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                   WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        return out;
    }

    struct Window {
        wl_surface* surface = nullptr;
        xdg_surface* xdg = nullptr;
        xdg_toplevel* toplevel = nullptr;
        xdg_popup* popup = nullptr;
    };

    /// A mapped window of `width`×`height`.
    Window window(int width, int height) {
        Window out;
        out.surface = wl_compositor_create_surface(compositor);
        out.xdg = xdg_wm_base_get_xdg_surface(wm_base, out.surface);
        add_ack(out.xdg);
        out.toplevel = xdg_surface_get_toplevel(out.xdg);
        wl_surface_commit(out.surface);
        pump();
        wl_surface_attach(out.surface, buffer(width, height), 0, 0);
        wl_surface_commit(out.surface);
        pump();
        return out;
    }

    /// A mapped popup anchored on `rect` of `parent`, opening downwards and
    /// flipping if it has to.
    Window popup(const Window& parent, lm::Box rect, int width, int height, bool grab = false) {
        Window out;
        out.surface = wl_compositor_create_surface(compositor);
        out.xdg = xdg_wm_base_get_xdg_surface(wm_base, out.surface);
        add_ack(out.xdg);
        xdg_positioner* positioner = xdg_wm_base_create_positioner(wm_base);
        xdg_positioner_set_size(positioner, width, height);
        xdg_positioner_set_anchor_rect(positioner, rect.x, rect.y, rect.width, rect.height);
        xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
        xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(positioner,
                                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
        out.popup = xdg_surface_get_popup(out.xdg, parent.xdg, positioner);
        xdg_positioner_destroy(positioner);
        static const xdg_popup_listener listener{
            [](void* data, xdg_popup*, std::int32_t, std::int32_t y, std::int32_t, std::int32_t) {
                static_cast<Client*>(data)->popup_y = y;
            },
            [](void* data, xdg_popup*) { ++static_cast<Client*>(data)->popups_done; },
            [](void*, xdg_popup*, std::uint32_t) {}};
        xdg_popup_add_listener(out.popup, &listener, this);
        if (grab) {
            xdg_popup_grab(out.popup, seat, last_serial);
        }
        wl_surface_commit(out.surface);
        pump();
        wl_surface_attach(out.surface, buffer(width, height), 0, 0);
        wl_surface_commit(out.surface);
        pump();
        return out;
    }

    /// Ask for a frame callback on the next commit of `surface`.
    void frame_callback(wl_surface* surface) {
        static const wl_callback_listener listener{
            [](void* data, wl_callback* callback, std::uint32_t) {
                ++static_cast<Client*>(data)->frames_done;
                wl_callback_destroy(callback);
            }};
        wl_callback_add_listener(wl_surface_frame(surface), &listener, this);
    }

    std::uint32_t last_serial = 0;

private:
    void add_ack(xdg_surface* surface) {
        static const xdg_surface_listener listener{
            [](void*, xdg_surface* xdg, std::uint32_t serial) {
                xdg_surface_ack_configure(xdg, serial);
            }};
        xdg_surface_add_listener(surface, &listener, this);
    }

    static void on_global(void* data, wl_registry* registry, std::uint32_t name,
                          const char* interface, std::uint32_t) {
        auto* self = static_cast<Client*>(data);
        const std::string_view iface{interface};
        if (iface == "wl_compositor") {
            self->compositor = static_cast<wl_compositor*>(
                wl_registry_bind(registry, name, &wl_compositor_interface, 4));
        } else if (iface == "wl_shm") {
            self->shm =
                static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        } else if (iface == "xdg_wm_base") {
            self->wm_base = static_cast<xdg_wm_base*>(
                wl_registry_bind(registry, name, &xdg_wm_base_interface, 5));
        } else if (iface == "wl_seat") {
            self->seat =
                static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 5));
            static const wl_seat_listener listener{on_seat_caps,
                                                   [](void*, wl_seat*, const char*) {}};
            wl_seat_add_listener(self->seat, &listener, self);
        } else if (iface == "wl_output") {
            self->output_globals.push_back(name);
            auto* output = static_cast<wl_output*>(
                wl_registry_bind(registry, name, &wl_output_interface, 4));
            static const wl_output_listener listener{
                [](void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                   std::int32_t, const char*, const char*, std::int32_t) {},
                [](void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {},
                [](void*, wl_output*) {},
                [](void*, wl_output*, std::int32_t) {},
                [](void* me, wl_output*, const char* output_name) {
                    static_cast<Client*>(me)->output_names.emplace_back(output_name);
                },
                [](void*, wl_output*, const char*) {}};
            wl_output_add_listener(output, &listener, self);
        }
    }
    static void on_global_remove(void* data, wl_registry*, std::uint32_t name) {
        std::erase(static_cast<Client*>(data)->output_globals, name);
    }
    static void on_seat_caps(void* data, wl_seat* seat, std::uint32_t caps) {
        auto* self = static_cast<Client*>(data);
        if ((caps & WL_SEAT_CAPABILITY_POINTER) != 0 && self->pointer == nullptr) {
            self->pointer = wl_seat_get_pointer(seat);
            static const wl_pointer_listener listener{
                [](void* me, wl_pointer*, std::uint32_t serial, wl_surface* surface, wl_fixed_t x,
                   wl_fixed_t y) {
                    auto* c = static_cast<Client*>(me);
                    ++c->pointer_enters;
                    c->last_serial = serial;
                    c->pointer_surface = surface;
                    c->pointer_x = wl_fixed_to_double(x);
                    c->pointer_y = wl_fixed_to_double(y);
                },
                [](void* me, wl_pointer*, std::uint32_t, wl_surface*) {
                    auto* c = static_cast<Client*>(me);
                    ++c->pointer_leaves;
                    c->pointer_surface = nullptr;
                },
                [](void* me, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
                    auto* c = static_cast<Client*>(me);
                    c->pointer_x = wl_fixed_to_double(x);
                    c->pointer_y = wl_fixed_to_double(y);
                },
                [](void* me, wl_pointer*, std::uint32_t serial, std::uint32_t, std::uint32_t button,
                   std::uint32_t state) {
                    auto* c = static_cast<Client*>(me);
                    c->last_serial = serial;
                    c->buttons.push_back(state == WL_POINTER_BUTTON_STATE_PRESSED
                                             ? button
                                             : 0x10000u | button);
                },
                [](void* me, wl_pointer*, std::uint32_t, std::uint32_t, wl_fixed_t value) {
                    static_cast<Client*>(me)->axis += wl_fixed_to_double(value);
                },
                [](void*, wl_pointer*) {},
                [](void*, wl_pointer*, std::uint32_t) {},
                [](void*, wl_pointer*, std::uint32_t, std::uint32_t) {},
                [](void*, wl_pointer*, std::uint32_t, std::int32_t) {}};
            wl_pointer_add_listener(self->pointer, &listener, self);
        }
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && self->keyboard == nullptr) {
            self->keyboard = wl_seat_get_keyboard(seat);
            static const wl_keyboard_listener listener{
                [](void*, wl_keyboard*, std::uint32_t, std::int32_t fd, std::uint32_t) {
                    close(fd);
                },
                [](void* me, wl_keyboard*, std::uint32_t, wl_surface* surface, wl_array*) {
                    static_cast<Client*>(me)->keyboard_surface = surface;
                },
                [](void* me, wl_keyboard*, std::uint32_t, wl_surface*) {
                    static_cast<Client*>(me)->keyboard_surface = nullptr;
                },
                [](void* me, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t key,
                   std::uint32_t state) {
                    static_cast<Client*>(me)->keys.push_back(
                        state == WL_KEYBOARD_KEY_STATE_PRESSED ? key : 0x10000u | key);
                },
                [](void*, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                   std::uint32_t) {},
                [](void*, wl_keyboard*, std::int32_t, std::int32_t) {}};
            wl_keyboard_add_listener(self->keyboard, &listener, self);
        }
    }
};

/// Fire `output` until nothing is asked of it any more, moving client traffic
/// between frames. A bound, so a test that keeps asking fails instead of hanging.
inline void settle(Client& client, ManualOutput& output) {
    for (int i = 0; i < 16; ++i) {
        client.pump();
        if (!output.fire() && !output.frame_scheduled()) {
            client.pump();
            return;
        }
    }
    client.pump();
}

} // namespace harness
