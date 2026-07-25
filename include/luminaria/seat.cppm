// luminaria/seat.cppm — the wl_seat: keyboard + pointer + touch input routing.
//
// The seat advertises input capabilities and routes events to the focused
// client. Keyboard focus is surface-scoped: keys go to whoever holds focus.
// Pointer focus follows the cursor. The compositor decides focus (from scene
// hit-testing) and calls the notify_*/pointer_* methods; the seat handles the
// wire events.
//
// Focus pointers are kept safe: the seat listens for Surface destruction and
// clears the focus itself, so a client destroying a focused surface can never
// leave a dangling pointer here.

module;

#include <cstdint>
#include <functional>
#include <typeinfo>
#include <memory>
#include <string>

export module luminaria:seat;

import :core.expected;
import :core.signal;

export namespace luminaria {

class Display;
class Surface;

/// Keyboard focus moved (surface may be null).
struct SeatKeyboardFocus {
    Surface* surface;
};
/// Pointer focus moved (surface may be null).
struct SeatPointerFocus {
    Surface* surface;
};
/// The pointer-focused client set its cursor image. `surface` is null when the
/// client asked to hide the cursor. The compositor composites it at the pointer
/// position minus the hotspot.
struct SeatCursorChange {
    Surface* surface;
    int hotspot_x;
    int hotspot_y;
};

/// Callbacks a drag-and-drop implementation (wl_data_device) installs on the
/// seat. While a drag is active the seat sends no wl_pointer events to clients;
/// it drives these instead, as the protocol requires.
struct SeatDragHooks {
    /// Pointer moved onto `surface` (null = left every surface).
    std::function<void(Surface* surface, double sx, double sy)> focus;
    std::function<void(double sx, double sy)> motion;
    /// Button released — the drop happens (or is cancelled) here.
    std::function<void()> drop;
};

class Seat {
public:
    /// Create the wl_seat global (keyboard + pointer capabilities) with an xkb
    /// keymap. Fails if the keymap can't be built.
    [[nodiscard]] static Result<Seat> create(Display& display, std::string name = "seat0");

    ~Seat();
    Seat(Seat&&) noexcept;
    Seat& operator=(Seat&&) noexcept;
    Seat(const Seat&) = delete;
    Seat& operator=(const Seat&) = delete;

    /// Re-advertise capabilities to every bound client. Defaults to keyboard +
    /// pointer; enable touch once a touch device actually exists.
    void set_capabilities(bool keyboard, bool pointer, bool touch);

    /// Replace the keyboard layout with an xkb keymap in text form, and re-send
    /// it to every bound wl_keyboard. Returns false if it doesn't compile, in
    /// which case the old one stays.
    ///
    /// This is what keeps a NESTED compositor's keys honest: the parent reports
    /// modifier masks computed against ITS keymap, and interpreting those
    /// against a locally-guessed one only works by luck on a US layout. Adopt
    /// `WaylandBackend::keymap()` and the two agree by construction.
    [[nodiscard]] bool set_keymap(const std::string& xkb_text);
    [[nodiscard]] const std::string& keymap() const noexcept;

    // --- keyboard ---
    /// Give keyboard focus to `surface` (nullptr clears focus). Sends leave/enter.
    void set_keyboard_focus(Surface* surface);
    [[nodiscard]] Surface* keyboard_focus() const noexcept;
    /// Send a key event to the keyboard-focused client. `pressed` = down.
    void notify_key(uint32_t key, bool pressed);
    /// Send the current modifier state (Shift/Ctrl/…) to the keyboard-focused client.
    void notify_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
    [[nodiscard]] Signal<SeatKeyboardFocus>& keyboard_focus_changed() noexcept;

    // --- pointer ---
    /// Move pointer focus onto `surface` at local (sx,sy) and send enter.
    void pointer_enter(Surface& surface, double sx, double sy);
    /// Drop pointer focus entirely (cursor left every surface): sends leave.
    void pointer_clear_focus();
    [[nodiscard]] Surface* pointer_focus() const noexcept;
    /// Send motion to the pointer-focused client.
    void pointer_motion(double sx, double sy);
    /// Send a button event to the pointer-focused client. `pressed` = down.
    void pointer_button(uint32_t button, bool pressed);
    /// Smooth (touchpad / high-resolution wheel) scrolling, in surface units.
    /// Positive dy scrolls down, positive dx scrolls right.
    void pointer_axis(double dx, double dy);
    /// Notched wheel scrolling: `steps` clicks along each axis.
    void pointer_axis_discrete(int32_t dx_steps, int32_t dy_steps);
    /// Scrolling finished (fingers lifted). Ends a smooth-scroll sequence.
    void pointer_axis_stop(bool horizontal, bool vertical);

    /// Fires when the focused client sets (or hides) its cursor image.
    [[nodiscard]] Signal<SeatCursorChange>& cursor_changed() noexcept;
    /// Current client cursor surface (null = hidden or never set).
    [[nodiscard]] Surface* cursor_surface() const noexcept;
    [[nodiscard]] int cursor_hotspot_x() const noexcept;
    [[nodiscard]] int cursor_hotspot_y() const noexcept;
    [[nodiscard]] Signal<SeatPointerFocus>& pointer_focus_changed() noexcept;

    // --- touch ---
    /// Begin a touch point on `surface` at surface-local (x,y). `id` is the slot.
    void touch_down(Surface& surface, int32_t id, double x, double y);
    void touch_motion(int32_t id, double x, double y);
    void touch_up(int32_t id);
    /// End the current set of touch events (send once per input frame).
    void touch_frame();
    /// The compositor took over the gesture; clients must forget it.
    void touch_cancel();

    // --- drag and drop (installed by wl_data_device) ---
    void begin_drag(SeatDragHooks hooks);
    void end_drag();
    [[nodiscard]] bool dragging() const noexcept;

    struct Impl; // named by the protocol glue

private:
    std::unique_ptr<Impl> impl_;
    explicit Seat(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
