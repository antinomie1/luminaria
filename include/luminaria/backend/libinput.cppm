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

export module luminaria:backend.libinput;

import :core.event_loop;
import :core.expected;
import :core.signal;
import :input_event;

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
