// luminaria/backend/libinput.hpp — bare-metal input via libinput. Emits keyboard and
// pointer events the compositor routes into a Seat.
//
// Requires permission to open /dev/input/event* (a logged-in VT session gets it
// via logind ACLs). TODO: no libseat — relies on session ACLs; codes are raw
// evdev (same as wl_keyboard/wl_pointer expect), so they pass straight to Seat.
//
// SAFETY: create() only builds the context; start() opens the devices (and thus
// grabs input). Never call start() under another compositor or you steal input.
#pragma once

#include <cstdint>
#include <memory>

#include "luminaria/core/event_loop.hpp"
#include "luminaria/core/expected.hpp"
#include "luminaria/core/signal.hpp"

namespace luminaria {

struct KeyEvent {
    uint32_t keycode; // evdev code (KEY_ESC == 1)
    bool pressed;
};
struct PointerMotionEvent {
    double dx, dy;
};
struct PointerButtonEvent {
    uint32_t button; // evdev code (BTN_LEFT == 272)
    bool pressed;
};

class LibinputBackend {
public:
    /// Build the libinput/udev context. Does NOT open any input device yet.
    [[nodiscard]] static Result<LibinputBackend> create(EventLoop loop);

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
