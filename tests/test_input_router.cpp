// DefaultInputRouter: input, end to end, given only what a key or a click means.
//
// A real client on a real window, and the events a backend would deliver:
//
//   - motion enters the window, in its own coordinates; absolute motion is
//     relative to the output it names, and leaving clears the focus;
//   - a binding keeps its key, release included, and is deaf to Caps Lock; the
//     hook is offered what no binding took, and the rest is the client's;
//   - a press reaches `on_button` with what is under the pointer before the
//     client hears of it, and gives the window the keyboard; a press in a popup
//     does not, and turning click-to-focus off leaves the keyboard alone;
//   - with a button held the pointer stays with the surface that took the press,
//     wherever it goes, and the release re-picks;
//   - a click outside a grabbing popup dismisses it;
//   - `ScreenManager::invalidate()` re-picks: the window under a still pointer
//     can change, and the client has to be told.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"

import luminaria.shell;
import std;

#include "shell_harness.h"

namespace {

namespace lm = luminaria;
using harness::Client;
using harness::ManualBackend;
using harness::ManualOutput;

constexpr int kW = 64, kH = 48;
constexpr std::uint32_t kKeyQ = 16, kKeyA = 30, kKeyB = 48, kKeyC = 46;
constexpr std::uint32_t kLeft = 0x110;

lm::KeyEvent key(std::uint32_t code, bool pressed, std::uint32_t keysym, std::uint32_t mods = 0) {
    return {.keycode = code, .pressed = pressed, .keysym = keysym, .modifiers = {mods, 0, 0, 0}};
}

/// An input source the test drives by hand, for `attach()`.
class FakeSource final : public lm::InputSource {
public:
    lm::Signal<lm::InputEvent>& input() noexcept override { return input_; }
    const std::string& keymap() const noexcept override { return keymap_; }
    lm::InputCapabilities capabilities() const noexcept override {
        return {.keyboard = true, .pointer = true};
    }
    void send(lm::InputEvent event) { input_.emit(event); }

private:
    lm::Signal<lm::InputEvent> input_;
    std::string keymap_;
};

} // namespace

int main() {
    setenv("VK_ICD_FILENAMES", "/nonexistent/luminaria-test-icd.json", 1); // nothing here draws
    setenv("VK_DRIVER_FILES", "/nonexistent/luminaria-test-icd.json", 1);
    auto display = lm::must(lm::Display::create(), "display");
    auto globals = lm::must(lm::CoreGlobals::create(display), "globals");
    lm::XdgShell& shell = globals.shell();
    lm::Seat& seat = globals.seat();
    lm::OutputLayout layout;
    lm::PopupTree popups(shell);
    ManualBackend backend;
    ManualOutput& output = backend.add(kW, kH);
    lm::SceneRenderer renderer;
    lm::Cursor cursor(renderer, layout, seat);

    std::vector<lm::Toplevel*> stack;
    std::vector<lm::SceneItem> scene;
    lm::ScreenManager screens(display, backend, renderer, layout);
    screens.follow_popups(popups);
    screens.set_scene_builder([&](lm::Output&) -> std::span<const lm::SceneItem> {
        scene.clear();
        // Stacked diagonally, so that one point can be on the top window alone.
        int at = 0;
        for (lm::Toplevel* t : stack) {
            scene.push_back(lm::toplevel_item(*t, at, 0));
            at += 8;
        }
        popups.append_to(scene);
        return scene;
    });

    lm::DefaultInputRouter input(seat, cursor, screens, shell, popups);
    FakeSource source;
    input.attach(source);

    lm::Connections connections;
    connections += shell.new_toplevel().connect([&](lm::NewToplevel& e) {
        lm::Toplevel* t = &e.toplevel;
        connections += t->map.connect([&, t] {
            stack.push_back(t);
            screens.invalidate();
        });
    });

    assert(backend.start());
    Client client(display);
    const Client::Window back = client.window(32, 32);
    const Client::Window front = client.window(32, 32); // at x = 8

    // The pointer starts in the middle of the first output, which is on the
    // front window: a window that maps under it is entered with nothing moving.
    assert(client.pointer_surface == front.surface);
    assert(client.pointer_x == 24.0 && client.pointer_y == 24.0);

    // Motion. Absolute names the output, relative carries on from there.
    const int enters = client.pointer_enters;
    source.send(lm::PointerMotionAbsEvent{.x = 4.0, .y = 6.0, .output = &output});
    client.pump();
    assert(client.pointer_surface == back.surface);
    assert(client.pointer_enters == enters + 1);
    assert(client.pointer_x == 4.0 && client.pointer_y == 6.0);
    source.send(lm::PointerMotionEvent{.dx = 1.0, .dy = 1.0});
    client.pump();
    assert(client.pointer_x == 5.0 && client.pointer_y == 7.0);
    source.send(lm::PointerMotionEvent{.dx = 7.0, .dy = 0.0}); // onto the front window
    client.pump();
    assert(client.pointer_surface == front.surface);
    assert(client.pointer_x == 4.0);

    // The keyboard goes where the compositor says; here, nowhere yet.
    assert(client.keyboard_surface == nullptr);
    int ran = 0, offered = 0;
    input.bind(lm::Mod::logo, XKB_KEY_q, [&] { ++ran; });
    input.on_key_binding([&](const lm::KeyEvent& e) {
        ++offered;
        return e.keycode == kKeyB;
    });

    // A click: the hook first, then the keyboard, then the client.
    std::vector<std::uint64_t> tags;
    std::vector<bool> pressed_seen;
    lm::SurfaceId hit_surface;
    int focus_changes = 0;
    input.on_focus_changed([&] { ++focus_changes; });
    input.on_button([&](std::uint32_t button, bool pressed, const lm::SceneHit& hit) {
        assert(button == kLeft);
        tags.push_back(hit.tag);
        pressed_seen.push_back(pressed);
        hit_surface = hit.surface != nullptr ? hit.surface->id() : lm::SurfaceId{};
    });
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = true});
    client.pump();
    assert(tags.size() == 1 && pressed_seen[0]);
    assert(tags[0] == reinterpret_cast<std::uintptr_t>(stack[1]));
    assert(hit_surface == stack[1]->surface().id());
    assert(client.buttons == std::vector<std::uint32_t>{kLeft});
    assert(client.keyboard_surface == front.surface);
    assert(focus_changes >= 1);
    assert(stack[1]->is_activated());

    // Held: the pointer stays with the window it pressed, even over nothing.
    source.send(lm::PointerMotionAbsEvent{.x = 60.0, .y = 40.0, .output = &output});
    client.pump();
    assert(client.pointer_surface == front.surface);
    assert(client.pointer_x == 52.0 && client.pointer_y == 40.0);
    const int leaves = client.pointer_leaves;
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = false});
    client.pump();
    assert(client.buttons.back() == (0x10000u | kLeft));
    assert(pressed_seen.size() == 2 && !pressed_seen[1]);
    assert(client.pointer_leaves == leaves + 1); // the release re-picked: nothing is there
    assert(client.pointer_surface == nullptr);

    // Keys. The client hears only what nobody kept, releases included.
    source.send(key(kKeyA, true, XKB_KEY_a));
    source.send(key(kKeyA, false, XKB_KEY_a));
    source.send(key(kKeyQ, true, XKB_KEY_q, lm::Mod::logo));
    source.send(key(kKeyQ, false, XKB_KEY_q, lm::Mod::logo));
    source.send(key(kKeyQ, true, XKB_KEY_Q, lm::Mod::logo | (1u << 1))); // Caps Lock: not held
    source.send(key(kKeyQ, false, XKB_KEY_Q, lm::Mod::logo));
    assert(ran == 2);
    assert(offered == 1); // A; a bound key does not get as far as the hook
    source.send(key(kKeyB, true, XKB_KEY_b));
    source.send(key(kKeyB, false, XKB_KEY_b));
    assert(offered == 2);
    source.send(key(kKeyC, true, XKB_KEY_q, lm::Mod::logo | lm::Mod::shift)); // not this binding
    source.send(key(kKeyC, false, XKB_KEY_q));
    client.pump();
    assert(ran == 2);
    assert((client.keys == std::vector<std::uint32_t>{kKeyA, 0x10000u | kKeyA, kKeyC,
                                                      0x10000u | kKeyC}));

    // Axis, through the same door.
    source.send(lm::PointerMotionAbsEvent{.x = 10.0, .y = 10.0, .output = &output});
    source.send(lm::PointerAxisEvent{.dy = 3.0});
    client.pump();
    assert(client.axis == 3.0);

    // A press in a popup leaves the keyboard on the window, and `tag` still names it.
    input.set_click_to_focus(true);
    const Client::Window menu = client.popup(front, {0, 0, 4, 4}, 8, 8);
    assert(client.popup_y >= 0);
    source.send(lm::PointerMotionAbsEvent{.x = 9.0, .y = 5.0, .output = &output});
    client.pump();
    assert(client.pointer_surface == menu.surface);
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = true});
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = false});
    client.pump();
    assert(tags[tags.size() - 2] == reinterpret_cast<std::uintptr_t>(stack[1]));
    assert(client.keyboard_surface == front.surface);

    // Click-to-focus off: the keyboard stays put, the click still arrives.
    input.set_click_to_focus(false);
    source.send(lm::PointerMotionAbsEvent{.x = 2.0, .y = 2.0, .output = &output});
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = true});
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = false});
    client.pump();
    assert(client.pointer_surface == back.surface);
    assert(client.keyboard_surface == front.surface);
    input.set_click_to_focus(true);
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = true});
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = false});
    client.pump();
    assert(client.keyboard_surface == back.surface);
    assert(stack[0]->is_activated() && !stack[1]->is_activated());

    // The window under a still pointer changes: only invalidate() can tell it.
    source.send(lm::PointerMotionAbsEvent{.x = 9.0, .y = 20.0, .output = &output});
    client.pump();
    assert(client.pointer_surface == front.surface);
    std::swap(stack[0], stack[1]); // `back` is on top now, at x = 8
    screens.invalidate();
    client.pump();
    assert(client.pointer_surface == back.surface);
    assert(client.pointer_x == 1.0);

    // A click outside a grabbing menu dismisses it.
    xdg_popup_destroy(menu.popup);
    xdg_surface_destroy(menu.xdg);
    wl_surface_destroy(menu.surface);
    client.pump();
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = true});
    client.pump();
    const Client::Window grabbing = client.popup(back, {0, 0, 4, 4}, 8, 8, /*grab=*/true);
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = false});
    source.send(lm::PointerMotionAbsEvent{.x = 60.0, .y = 46.0, .output = &output});
    client.pump();
    assert(client.popups_done == 0);
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = true});
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = false});
    client.pump();
    assert(client.popups_done == 1);
    (void)grabbing;

    // Leaving: no pointer, no focus, and a button goes nowhere.
    source.send(lm::PointerMotionAbsEvent{.x = 9.0, .y = 20.0, .output = &output});
    client.pump();
    assert(client.pointer_surface != nullptr);
    const std::size_t buttons = client.buttons.size();
    source.send(lm::PointerLeaveEvent{});
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = true});
    source.send(lm::PointerButtonEvent{.button = kLeft, .pressed = false});
    client.pump();
    assert(client.pointer_surface == nullptr);
    assert(client.buttons.size() == buttons);

    return 0;
}
