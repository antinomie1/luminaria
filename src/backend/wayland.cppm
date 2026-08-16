// luminaria/backend/wayland.cppm — nested backend: run as a client inside a parent
// Wayland compositor. Each output is a parent window (xdg_toplevel); frames are
// driven by the parent's frame callbacks.
//
// Requires a running parent compositor (WAYLAND_DISPLAY). There are two ways to
// present: `commit_scanout()` hands the parent a dmabuf the renderer drew into
// (zero copy — nothing is ever read back or memcpy'd), and `commit_frame()`
// packs CPU pixels into a fresh wl_shm buffer. The first needs the parent to
// implement zwp_linux_dmabuf_v1; `scanout_modifiers()` returns an empty list
// when it does not, which is how a caller knows to take the shm path instead.

module;


#include <cstdint>
#include <cstring>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

export module luminaria:wayland;

import std;

import :backend;
import :color;
import :dmabuf;
import :event_loop;
import :expected;
import :input_event;
import :output;
import :pixel;
import :signal;

export namespace luminaria {

/// Who draws the frame around OUR nested window, as negotiated with the parent
/// compositor via xdg-decoration-unstable-v1. The mirror image of
/// luminaria::DecorationMode in luminaria/xdg_decoration.cppm, which is the
/// same question asked by our own clients.
/// "The parent compositor's keyboard layout is this." `text` is an xkb keymap
/// in text form, borrowed for the duration of the emit.
struct KeymapChange {
    const std::string& text;
};

enum class HostDecorationMode {
    None,       ///< the parent has no xdg-decoration global: no frame at all
    ClientSide, ///< the parent insists WE draw it (GTK-style CSD); we don't
    ServerSide, ///< the parent draws a native titlebar around us — what we ask for
};

class WaylandBackend final : public Backend {
public:
    /// Connect to the parent compositor and bind its globals. Fails if there is
    /// no parent (WAYLAND_DISPLAY) or it lacks wl_compositor/xdg_wm_base/wl_shm.
    [[nodiscard]] static Result<WaylandBackend> create(EventLoop loop);

    ~WaylandBackend();
    WaylandBackend(WaylandBackend&&) noexcept;
    WaylandBackend& operator=(WaylandBackend&&) noexcept;
    WaylandBackend(const WaylandBackend&) = delete;
    WaylandBackend& operator=(const WaylandBackend&) = delete;

    /// Create a parent window of the given size, titled `title`. The window
    /// asks the parent compositor for a native (server-side) decoration, so it
    /// looks like any other application on the host desktop. Call before start().
    Output& add_output(int width, int height, std::string title = "luminaria");

    Status start() override;

    /// What the parent granted. Only meaningful after start() — the negotiation
    /// completes with the first configure. `None` means the parent doesn't
    /// implement xdg-decoration at all (e.g. GNOME/Mutter), so the nested window
    /// is bare; `ClientSide` means it refused to decorate us.
    [[nodiscard]] HostDecorationMode decoration_mode() const noexcept;

    // Input forwarded from the parent compositor (cursor is over our window).
    // Coordinates are output-local; the compositor hit-tests and routes to a
    // client surface. Pointer leave clears focus (motion x/y set to -1,-1).
    Signal<KeyEvent> key;
    Signal<ModifiersEvent> modifiers;
    /// The parent compositor told us its keyboard layout. Hand `text` to
    /// `Seat::set_keymap()` so our clients see the same layout the user is
    /// actually typing on — otherwise the modifier masks forwarded above are
    /// interpreted against a different keymap and non-US layouts misbehave.
    Signal<KeymapChange> keymap_changed;

    /// The parent's xkb keymap in text form; empty until it sends one.
    [[nodiscard]] const std::string& keymap() const noexcept;
    Signal<PointerMotionAbsEvent> pointer_motion;
    Signal<PointerButtonEvent> pointer_button;
    /// One scroll frame, accumulated across the parent's axis events.
    Signal<PointerAxisEvent> pointer_axis;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit WaylandBackend(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

namespace {

uint8_t to_u8(float c) {
    const float v = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

// Per-buffer state, freed when the parent releases the wl_buffer.
struct ShmBuffer {
    wl_buffer* buffer;
    void* data;
    size_t size;
};
void buffer_release(void* data, wl_buffer* buffer) {
    auto* sb = static_cast<ShmBuffer*>(data);
    wl_buffer_destroy(buffer);
    munmap(sb->data, sb->size);
    delete sb;
}
const wl_buffer_listener kBufferListener{buffer_release};

// Allocate an XRGB8888 shm buffer; on success sets *out_pixels to the w*h u32
// mapping for the caller to fill. The buffer frees itself on parent release.
wl_buffer* make_shm_buffer(wl_shm* shm, int w, int h, uint32_t** out_pixels) {
    const int stride = w * 4;
    const size_t size = static_cast<size_t>(stride) * h;
    int fd = memfd_create("luminaria-nested", MFD_CLOEXEC);
    if (fd < 0) {
        return nullptr;
    }
    wl_buffer* buffer = nullptr;
    if (ftruncate(fd, static_cast<off_t>(size)) == 0) {
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) {
            wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(size));
            buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
            wl_shm_pool_destroy(pool);
            auto* sb = new ShmBuffer{buffer, p, size};
            wl_buffer_add_listener(buffer, &kBufferListener, sb);
            *out_pixels = static_cast<uint32_t*>(p);
        }
    }
    close(fd);
    return buffer;
}

wl_buffer* make_solid_buffer(wl_shm* shm, int w, int h, Color color) {
    uint32_t* px = nullptr;
    wl_buffer* buffer = make_shm_buffer(shm, w, h, &px);
    if (buffer == nullptr) {
        return nullptr;
    }
    const uint32_t argb = (static_cast<uint32_t>(to_u8(color.r)) << 16) |
                          (static_cast<uint32_t>(to_u8(color.g)) << 8) |
                          static_cast<uint32_t>(to_u8(color.b));
    std::fill_n(px, static_cast<size_t>(w) * h, argb);
    return buffer;
}

class WaylandOutput final : public Output {
public:
    /// Kick a frame from outside (the very first one, before the parent's
    /// callback chain exists).
    void kick_frame() { emit_software_frame(); }

    wl_display* parent;
    wl_shm* shm;
    wl_surface* surface;
    // The parent's linux-dmabuf, if it has one, and every (format, modifier)
    // pair it advertised. Copied from the backend at add_output(): the registry
    // roundtrip in create() is already done by then, so the list is complete.
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    std::vector<std::pair<uint32_t, uint64_t>> dmabuf_formats;
    // Buffers wrapping renderer scanout targets; the index is the scanout id.
    std::vector<wl_buffer*> scanout_buffers;
    xdg_surface* xsurf = nullptr;
    xdg_toplevel* toplevel = nullptr;
    zxdg_toplevel_decoration_v1* decoration = nullptr;
    bool configured = false;
    // True once the decoration has told us who draws the frame. Starts true so
    // a parent without xdg-decoration never makes start() wait.
    bool decoration_configured = true;
    HostDecorationMode decoration_mode = HostDecorationMode::None;

    WaylandOutput(wl_display* parent, wl_shm* shm, wl_surface* surface, int w, int h)
        : Output(w, h), parent(parent), shm(shm), surface(surface) {}

    ~WaylandOutput() override {
        // The dmabufs themselves belong to the renderer's scanout targets; only
        // the parent-side wl_buffer wrappers are ours.
        for (wl_buffer* buffer : scanout_buffers) {
            if (buffer != nullptr) {
                wl_buffer_destroy(buffer);
            }
        }
        // Innermost first: the decoration hangs off the toplevel.
        if (decoration != nullptr) {
            zxdg_toplevel_decoration_v1_destroy(decoration);
        }
        if (toplevel != nullptr) {
            xdg_toplevel_destroy(toplevel);
        }
        if (xsurf != nullptr) {
            xdg_surface_destroy(xsurf);
        }
        if (surface != nullptr) {
            wl_surface_destroy(surface);
        }
    }

    Status commit(Color color) override {
        wl_buffer* buffer = make_solid_buffer(shm, width_, height_, color);
        if (buffer == nullptr) {
            return fail("nested: buffer allocation failed");
        }
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, width_, height_);
        request_frame();
        wl_surface_commit(surface);
        wl_display_flush(parent);
        return ok();
    }

    // Present a composited RGBA frame: pack into an XRGB8888 shm buffer and
    // hand it to the parent. This is what makes client windows visible nested.
    Status commit_frame(std::span<const Pixel> rgba, int w, int h) override {
        if (w != width_ || h != height_ ||
            rgba.size() < static_cast<size_t>(w) * static_cast<size_t>(h)) {
            return fail("nested: frame size mismatch");
        }
        uint32_t* px = nullptr;
        wl_buffer* buffer = make_shm_buffer(shm, w, h, &px);
        if (buffer == nullptr) {
            return fail("nested: buffer allocation failed");
        }
        const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
        for (size_t i = 0; i < n; ++i) {
            px[i] = (static_cast<uint32_t>(rgba[i].r) << 16) |
                    (static_cast<uint32_t>(rgba[i].g) << 8) |
                    static_cast<uint32_t>(rgba[i].b);
        }
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, w, h);
        request_frame();
        wl_surface_commit(surface);
        wl_display_flush(parent);
        return ok();
    }

    // --- zero-copy path: hand the parent the renderer's dmabuf directly ---

    /// Every modifier the parent advertised for this format, and nothing else:
    /// what the parent will accept is not ours to guess. Empty when the parent
    /// has no linux-dmabuf at all, which is what tells the caller to take the
    /// shm path instead.
    [[nodiscard]] std::vector<std::uint64_t> scanout_modifiers(std::uint32_t drm_format) override {
        std::vector<std::uint64_t> mods;
        for (const auto& [format, modifier] : dmabuf_formats) {
            if (format == drm_format) {
                mods.push_back(modifier);
            }
        }
        return mods;
    }

    [[nodiscard]] Result<std::uint32_t> import_scanout(const DmabufPlane& plane) override {
        if (dmabuf == nullptr) {
            return fail("nested: parent has no zwp_linux_dmabuf_v1");
        }
        zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(dmabuf);
        // `plane.fd` is borrowed — libwayland-client sends a copy over the
        // socket and leaves ours alone, so there is nothing to close here.
        zwp_linux_buffer_params_v1_add(params, plane.fd, 0, plane.offset, plane.stride,
                                       static_cast<uint32_t>(plane.modifier >> 32),
                                       static_cast<uint32_t>(plane.modifier & 0xffffffffu));
        // create_immed (v2+) rather than create: no round trip, and a parent
        // that dislikes the buffer kills the connection instead of replying,
        // which is the right outcome for a bug on our side.
        wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(
            params, plane.width, plane.height, plane.format, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        if (buffer == nullptr) {
            return fail("nested: zwp_linux_buffer_params_v1.create_immed failed");
        }
        scanout_buffers.push_back(buffer);
        return static_cast<std::uint32_t>(scanout_buffers.size() - 1);
    }

    /// The slot is left in place rather than compacted: ids are indices, and
    /// renumbering them would invalidate every id the caller still holds.
    void release_scanout(std::uint32_t id) override {
        if (id < scanout_buffers.size() && scanout_buffers[id] != nullptr) {
            wl_buffer_destroy(scanout_buffers[id]);
            scanout_buffers[id] = nullptr;
        }
    }

    Status commit_scanout(std::uint32_t id, int in_fence_fd) override {
        if (id >= scanout_buffers.size() || scanout_buffers[id] == nullptr) {
            if (in_fence_fd >= 0) {
                close(in_fence_fd);
            }
            return fail("nested: unknown scanout id");
        }
        if (in_fence_fd >= 0) {
            // There is no explicit-sync channel to the parent on this path
            // (linux-drm-syncobj is a compositor-side protocol, and we are the
            // client here), so the wait has to happen on our CPU. Callers that
            // do not ask render_to() for a fence never get here: it blocks
            // internally and passes -1.
            pollfd pfd{in_fence_fd, POLLIN, 0};
            poll(&pfd, 1, 1000);
            close(in_fence_fd);
        }
        wl_surface_attach(surface, scanout_buffers[id], 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, width_, height_);
        request_frame();
        wl_surface_commit(surface);
        wl_display_flush(parent);
        return ok();
    }

    void request_frame() {
        wl_callback* cb = wl_surface_frame(surface);
        wl_callback_add_listener(cb, &frame_listener_, this);
    }

protected:
    // Frames here are the parent's to pace, and it only owes us one when we
    // have asked. A commit carrying no new buffer is exactly that ask: it costs
    // no allocation, changes nothing on screen, and comes back on the parent's
    // vblank rather than on a timer of our own.
    void arm_frame() override {
        if (!configured || surface == nullptr) {
            return; // the window does not exist yet; start() kicks the first frame
        }
        request_frame();
        wl_surface_commit(surface);
        wl_display_flush(parent);
    }

private:
    static void on_frame(void* data, wl_callback* cb, uint32_t) {
        auto* self = static_cast<WaylandOutput*>(data);
        wl_callback_destroy(cb);
        // The parent compositor already paced us to its vblank; report the
        // arrival time as the presentation time.
        self->emit_software_frame();
    }
    static constexpr wl_callback_listener frame_listener_{on_frame};
};

} // namespace

struct WaylandBackend::Impl {
    EventLoop loop;
    wl_display* parent = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
    // Optional: the parent may be shm-only. Filled by the listener below.
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    std::vector<std::pair<uint32_t, uint64_t>> dmabuf_formats;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    zxdg_decoration_manager_v1* decoration_manager = nullptr; // null if the parent lacks it
    WaylandBackend* owner = nullptr; // set in start(), after all moves; emits input signals
    std::string keymap;              // the parent's xkb keymap, verbatim
    PointerAxisEvent axis;           // accumulated until wl_pointer.frame
    bool axis_pending = false;
    EventSource fd_source;
    std::vector<std::unique_ptr<WaylandOutput>> outputs;

    ~Impl() {
        outputs.clear();
        if (keyboard != nullptr) {
            wl_keyboard_destroy(keyboard);
        }
        if (pointer != nullptr) {
            wl_pointer_destroy(pointer);
        }
        if (seat != nullptr) {
            wl_seat_destroy(seat);
        }
        if (decoration_manager != nullptr) {
            zxdg_decoration_manager_v1_destroy(decoration_manager);
        }
        if (wm_base != nullptr) {
            xdg_wm_base_destroy(wm_base);
        }
        if (dmabuf != nullptr) {
            zwp_linux_dmabuf_v1_destroy(dmabuf);
        }
        if (shm != nullptr) {
            wl_shm_destroy(shm);
        }
        if (compositor != nullptr) {
            wl_compositor_destroy(compositor);
        }
        if (registry != nullptr) {
            wl_registry_destroy(registry);
        }
        if (parent != nullptr) {
            wl_display_disconnect(parent);
        }
    }
};

namespace {

void wm_base_ping(void*, xdg_wm_base* wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}
const xdg_wm_base_listener kWmBaseListener{wm_base_ping};

// ---- parent input: forward pointer/keyboard into the backend's signals ----
// data is Impl*; owner is set by the time these fire (start() ran first).
void emit_motion(WaylandBackend::Impl* impl, double x, double y) {
    if (impl->owner != nullptr) {
        PointerMotionAbsEvent e{x, y};
        impl->owner->pointer_motion.emit(e);
    }
}
void ptr_enter(void* data, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t sx, wl_fixed_t sy) {
    emit_motion(static_cast<WaylandBackend::Impl*>(data), wl_fixed_to_double(sx),
                wl_fixed_to_double(sy));
}
void ptr_leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
    emit_motion(static_cast<WaylandBackend::Impl*>(data), -1.0, -1.0); // clear focus
}
void ptr_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    emit_motion(static_cast<WaylandBackend::Impl*>(data), wl_fixed_to_double(sx),
                wl_fixed_to_double(sy));
}
void ptr_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if (impl->owner != nullptr) {
        PointerButtonEvent e{button, state == WL_POINTER_BUTTON_STATE_PRESSED};
        impl->owner->pointer_button.emit(e);
    }
}
// Scroll arrives as several events terminated by wl_pointer.frame; accumulate
// and emit one PointerAxisEvent per frame so the compositor scrolls once.
void ptr_axis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    const double v = wl_fixed_to_double(value);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        impl->axis.dy += v;
    } else {
        impl->axis.dx += v;
    }
    impl->axis_pending = true;
}
void ptr_axis_source(void*, wl_pointer*, uint32_t) {}
void ptr_axis_stop(void* data, wl_pointer*, uint32_t, uint32_t axis) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        impl->axis.stop_vertical = true;
    } else {
        impl->axis.stop_horizontal = true;
    }
    impl->axis_pending = true;
}
void ptr_axis_discrete(void* data, wl_pointer*, uint32_t axis, int32_t discrete) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        impl->axis.dy_steps += discrete;
    } else {
        impl->axis.dx_steps += discrete;
    }
    impl->axis_pending = true;
}
void ptr_frame(void* data, wl_pointer*) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if (!impl->axis_pending) {
        return;
    }
    PointerAxisEvent e = impl->axis;
    impl->axis = PointerAxisEvent{};
    impl->axis_pending = false;
    if (impl->owner != nullptr) {
        impl->owner->pointer_axis.emit(e);
    }
}
const wl_pointer_listener kPointerListener{ptr_enter,       ptr_leave,     ptr_motion,
                                           ptr_button,      ptr_axis,      ptr_frame,
                                           ptr_axis_source, ptr_axis_stop, ptr_axis_discrete};

// Keep the parent's keymap rather than throwing it away. The modifier masks the
// parent reports below are computed against THIS keymap; interpreting them
// against a locally-guessed one only works by accident on a US layout.
void kb_keymap(void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0) {
        void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p != MAP_FAILED) {
            // The payload is NUL-terminated; don't take the terminator itself.
            const char* text = static_cast<const char*>(p);
            impl->keymap.assign(text, strnlen(text, size));
            munmap(p, size);
            if (impl->owner != nullptr) {
                KeymapChange e{impl->keymap};
                impl->owner->keymap_changed.emit(e);
            }
        }
    }
    close(fd);
}
void kb_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
void kb_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
void kb_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if (impl->owner != nullptr) {
        KeyEvent e{key, state == WL_KEYBOARD_KEY_STATE_PRESSED};
        impl->owner->key.emit(e);
    }
}
void kb_mods(void* data, wl_keyboard*, uint32_t, uint32_t dep, uint32_t lat, uint32_t lock,
             uint32_t grp) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if (impl->owner != nullptr) {
        ModifiersEvent e{dep, lat, lock, grp};
        impl->owner->modifiers.emit(e);
    }
}
void kb_repeat(void*, wl_keyboard*, int32_t, int32_t) {}
const wl_keyboard_listener kKeyboardListener{kb_keymap, kb_enter, kb_leave,
                                             kb_key,    kb_mods,  kb_repeat};

void seat_caps(void* data, wl_seat* seat, uint32_t caps) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) != 0 && impl->pointer == nullptr) {
        impl->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(impl->pointer, &kPointerListener, impl);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && impl->keyboard == nullptr) {
        impl->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(impl->keyboard, &kKeyboardListener, impl);
    }
}
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener kSeatListener{seat_caps, seat_name};

// zwp_linux_dmabuf_v1 v3 advertises what it can import as a burst of `modifier`
// events at bind time. The bare `format` event is the v1 form and carries no
// modifier; v3 compositors do not send it, but the slot must not be null.
void dmabuf_format(void*, zwp_linux_dmabuf_v1*, uint32_t) {}
void dmabuf_modifier(void* data, zwp_linux_dmabuf_v1*, uint32_t format, uint32_t hi, uint32_t lo) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    impl->dmabuf_formats.emplace_back(format,
                                      (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo));
}
const zwp_linux_dmabuf_v1_listener kDmabufListener{dmabuf_format, dmabuf_modifier};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t version) {
    auto* impl = static_cast<WaylandBackend::Impl*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        impl->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, "xdg_wm_base") == 0) {
        impl->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(impl->wm_base, &kWmBaseListener, impl);
    } else if (std::strcmp(interface, "wl_shm") == 0) {
        impl->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
        // Cap at 3: that is the last version that advertises formats up front.
        // v4 replaces them with per-surface feedback, which a nested window
        // does not need — and binding 3 against a v4 global is legal.
        if (version >= 3) {
            impl->dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
                wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3));
            zwp_linux_dmabuf_v1_add_listener(impl->dmabuf, &kDmabufListener, impl);
        }
    } else if (std::strcmp(interface, "zxdg_decoration_manager_v1") == 0) {
        // Optional: only compositors that do server-side decorations advertise it.
        impl->decoration_manager = static_cast<zxdg_decoration_manager_v1*>(
            wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1));
    } else if (std::strcmp(interface, "wl_seat") == 0) {
        // Bind v5: that is what gives us scroll (axis_source/discrete/stop) and
        // wl_pointer.frame. Every listener slot up to v5 is filled below —
        // libwayland aborts on a null slot for an event the parent sends.
        impl->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 5));
        wl_seat_add_listener(impl->seat, &kSeatListener, impl);
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

void xdg_surface_configure(void* data, xdg_surface* xsurf, uint32_t serial) {
    auto* out = static_cast<WaylandOutput*>(data);
    xdg_surface_ack_configure(xsurf, serial);
    out->configured = true;
    wl_display_flush(out->parent);
}
const xdg_surface_listener kXdgSurfaceListener{xdg_surface_configure};

void toplevel_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
void toplevel_close(void*, xdg_toplevel*) {}
const xdg_toplevel_listener kToplevelListener{toplevel_configure, toplevel_close};

// The parent's answer to our set_mode: it may hand us SERVER_SIDE (what we
// asked for — a native titlebar) or insist on CLIENT_SIDE.
void decoration_configure(void* data, zxdg_toplevel_decoration_v1*, uint32_t mode) {
    auto* out = static_cast<WaylandOutput*>(data);
    out->decoration_mode = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                               ? HostDecorationMode::ServerSide
                               : HostDecorationMode::ClientSide;
    out->decoration_configured = true;
}
const zxdg_toplevel_decoration_v1_listener kDecorationListener{decoration_configure};

} // namespace

WaylandBackend::WaylandBackend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
WaylandBackend::~WaylandBackend() = default;
WaylandBackend::WaylandBackend(WaylandBackend&&) noexcept = default;
WaylandBackend& WaylandBackend::operator=(WaylandBackend&&) noexcept = default;

Result<WaylandBackend> WaylandBackend::create(EventLoop loop) {
    wl_display* parent = wl_display_connect(nullptr);
    if (parent == nullptr) {
        return fail("nested: no parent compositor (WAYLAND_DISPLAY)");
    }
    auto impl = std::make_unique<Impl>();
    impl->loop = loop;
    impl->parent = parent;
    impl->registry = wl_display_get_registry(parent);
    wl_registry_add_listener(impl->registry, &kRegistryListener, impl.get());
    wl_display_roundtrip(parent); // receive globals
    // A second one: the seat capabilities and the linux-dmabuf modifier list
    // are only sent once we have bound those globals, which happened during
    // the roundtrip above.
    wl_display_roundtrip(parent);

    if (impl->compositor == nullptr || impl->wm_base == nullptr || impl->shm == nullptr) {
        return fail("nested: parent lacks wl_compositor/xdg_wm_base/wl_shm");
    }
    return WaylandBackend{std::move(impl)};
}

Output& WaylandBackend::add_output(int width, int height, std::string title) {
    auto out = std::make_unique<WaylandOutput>(impl_->parent, impl_->shm,
                                               wl_compositor_create_surface(impl_->compositor),
                                               width, height);
    out->dmabuf = impl_->dmabuf;
    out->dmabuf_formats = impl_->dmabuf_formats;
    out->xsurf = xdg_wm_base_get_xdg_surface(impl_->wm_base, out->surface);
    xdg_surface_add_listener(out->xsurf, &kXdgSurfaceListener, out.get());
    out->toplevel = xdg_surface_get_toplevel(out->xsurf);
    xdg_toplevel_add_listener(out->toplevel, &kToplevelListener, out.get());
    xdg_toplevel_set_title(out->toplevel, title.c_str());
    // app_id is what the host desktop keys its icon and window grouping off.
    xdg_toplevel_set_app_id(out->toplevel, "org.luminaria.compositor");

    // Ask the parent for a native titlebar. Must be requested before the first
    // buffer: the answering configure arrives with the xdg_surface configure,
    // and start() waits for both.
    if (impl_->decoration_manager != nullptr) {
        out->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            impl_->decoration_manager, out->toplevel);
        zxdg_toplevel_decoration_v1_add_listener(out->decoration, &kDecorationListener, out.get());
        zxdg_toplevel_decoration_v1_set_mode(out->decoration,
                                             ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        out->decoration_configured = false;
    }

    wl_surface_commit(out->surface); // initial commit -> parent sends configure
    wl_display_flush(impl_->parent);

    Output& ref = *out;
    impl_->outputs.push_back(std::move(out));
    return ref;
}

Status WaylandBackend::start() {
    impl_->owner = this; // this object is now at its final address; input can emit
    // Drive the parent connection from our event loop.
    impl_->fd_source = impl_->loop.add_fd(wl_display_get_fd(impl_->parent), [this] {
        if (wl_display_dispatch(impl_->parent) < 0) {
            impl_->fd_source.disarm(); // parent gone; stop polling
        }
    });

    // Block until every output has had its first configure — including the
    // decoration's, so we never attach a buffer before the frame is settled.
    for (int tries = 0; tries < 100; ++tries) {
        const bool all =
            std::ranges::all_of(impl_->outputs, [](const std::unique_ptr<WaylandOutput>& o) {
                return o->configured && o->decoration_configured;
            });
        if (all) {
            break;
        }
        wl_display_roundtrip(impl_->parent);
    }

    for (auto& out : impl_->outputs) {
        NewOutput event{*out};
        new_output.emit(event);
    }
    // Kick the first frame so the compositor commits initial content (which maps
    // the window); subsequent frames come from parent frame callbacks.
    for (auto& out : impl_->outputs) {
        WaylandOutput* raw = out.get();
        impl_->loop.once([raw] { raw->kick_frame(); });
    }
    wl_display_flush(impl_->parent);
    return ok();
}

const std::string& WaylandBackend::keymap() const noexcept { return impl_->keymap; }

HostDecorationMode WaylandBackend::decoration_mode() const noexcept {
    if (impl_->outputs.empty()) {
        return HostDecorationMode::None;
    }
    return impl_->outputs.front()->decoration_mode;
}

} // namespace luminaria
