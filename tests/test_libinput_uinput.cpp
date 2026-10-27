// libinput backend against REAL kernel input devices: two uinput devices are
// created, handed to a path-backend LibinputBackend, and fed actual evdev
// events. This is the only test that proves the libinput half works at all —
// everything else about that file is unobservable without a device.
//
// It needs read/write access to /dev/input/event*, which on most systems means
// the `input` group (or root). Without it there is nothing to test, so it skips
// with 77 rather than failing.
//
// The events injected here are deliberately the harmless ones. A uinput device
// is visible to EVERY input consumer on the machine, so anything this test
// types also reaches whatever the user has focused: Shift on its own produces
// no character, a five-pixel pointer move and one wheel notch are recoverable,
// and a button press — which would click whatever the cursor happens to be
// over — is never sent.
//
// A touchscreen cannot be made harmless the same way: the desktop maps it onto
// its own screen, and a touch there is a tap on whatever is under the finger.
// So that half only runs when asked for with LUMINARIA_TEST_TOUCH=1 — on a CI
// machine with no desktop, or one where a tap near the left middle of the
// screen is known to be safe.
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

import luminaria.gpu;
import std;

namespace {

void sleep_ms(long ms) {
    struct timespec ts{ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&ts, nullptr);
}

/// A virtual kernel input device. `node` is the /dev/input/eventN the kernel
/// created for it, which is what libinput opens.
struct Uinput {
    int fd = -1;
    std::string node;

    void emit(uint16_t type, uint16_t code, int32_t value) const {
        struct input_event ev {};
        ev.type = type;
        ev.code = code;
        ev.value = value;
        [[maybe_unused]] const ssize_t written = write(fd, &ev, sizeof ev);
    }
    /// Every logical input event ends with a SYN_REPORT; libinput ignores
    /// anything that hasn't been reported yet.
    void sync() const { emit(EV_SYN, SYN_REPORT, 0); }

    void destroy() {
        if (fd >= 0) {
            ioctl(fd, UI_DEV_DESTROY);
            close(fd);
            fd = -1;
        }
    }
};

/// The eventN node belonging to a just-created uinput device, once udev has
/// finished writing its properties — libinput classifies devices from those, so
/// adding the device before udev has caught up gets it misidentified.
std::string node_of(int fd) {
    char sysname[128] = {};
    if (ioctl(fd, UI_GET_SYSNAME(sizeof sysname), sysname) < 0) {
        return {};
    }
    const std::string dir = std::string("/sys/class/input/") + sysname;
    std::string node;
    for (int tries = 0; tries < 100 && node.empty(); ++tries) {
        if (DIR* d = opendir(dir.c_str()); d != nullptr) {
            while (dirent* e = readdir(d)) {
                if (std::strncmp(e->d_name, "event", 5) == 0) {
                    node = std::string("/dev/input/") + e->d_name;
                    break;
                }
            }
            closedir(d);
        }
        if (node.empty()) {
            sleep_ms(20);
        }
    }
    if (node.empty()) {
        return {};
    }
    struct stat st {};
    if (stat(node.c_str(), &st) == 0) {
        // udev stores what it learned about the device here. Its presence is
        // the signal that ID_INPUT_KEYBOARD/ID_INPUT_MOUSE have been set.
        char db[64];
        std::snprintf(db, sizeof db, "/run/udev/data/c%u:%u", major(st.st_rdev), minor(st.st_rdev));
        for (int tries = 0; tries < 100 && access(db, F_OK) != 0; ++tries) {
            sleep_ms(20);
        }
    }
    return node;
}

Uinput make_device(const char* name, const std::vector<uint16_t>& keys,
                   const std::vector<uint16_t>& rels) {
    Uinput device;
    device.fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (device.fd < 0) {
        return device;
    }
    ioctl(device.fd, UI_SET_EVBIT, EV_KEY);
    for (uint16_t key : keys) {
        ioctl(device.fd, UI_SET_KEYBIT, key);
    }
    if (!rels.empty()) {
        ioctl(device.fd, UI_SET_EVBIT, EV_REL);
        for (uint16_t rel : rels) {
            ioctl(device.fd, UI_SET_RELBIT, rel);
        }
    }
    struct uinput_setup setup {};
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x4c55;  // "LU"
    setup.id.product = 0x4d49; // "MI"
    std::snprintf(setup.name, sizeof setup.name, "%s", name);
    if (ioctl(device.fd, UI_DEV_SETUP, &setup) < 0 || ioctl(device.fd, UI_DEV_CREATE) < 0) {
        close(device.fd);
        device.fd = -1;
        return device;
    }
    device.node = node_of(device.fd);
    return device;
}

/// A two-finger multitouch screen (MT protocol B), 0..1000 on both axes.
/// INPUT_PROP_DIRECT + ABS_X/Y + BTN_TOUCH is what udev calls a touchscreen.
Uinput make_touchscreen(const char* name) {
    Uinput device;
    device.fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (device.fd < 0) {
        return device;
    }
    ioctl(device.fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    ioctl(device.fd, UI_SET_EVBIT, EV_KEY);
    ioctl(device.fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(device.fd, UI_SET_EVBIT, EV_ABS);
    const auto axis = [&](uint16_t code, int32_t min, int32_t max, int32_t resolution) {
        ioctl(device.fd, UI_SET_ABSBIT, code);
        struct uinput_abs_setup abs {};
        abs.code = code;
        abs.absinfo.minimum = min;
        abs.absinfo.maximum = max;
        abs.absinfo.resolution = resolution; // units per mm: a 100 mm panel
        ioctl(device.fd, UI_ABS_SETUP, &abs);
    };
    axis(ABS_X, 0, 1000, 10);
    axis(ABS_Y, 0, 1000, 10);
    axis(ABS_MT_SLOT, 0, 1, 0);
    axis(ABS_MT_TRACKING_ID, 0, 65535, 0);
    axis(ABS_MT_POSITION_X, 0, 1000, 10);
    axis(ABS_MT_POSITION_Y, 0, 1000, 10);
    struct uinput_setup setup {};
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x4c55;
    setup.id.product = 0x4d4a;
    std::snprintf(setup.name, sizeof setup.name, "%s", name);
    if (ioctl(device.fd, UI_DEV_SETUP, &setup) < 0 || ioctl(device.fd, UI_DEV_CREATE) < 0) {
        close(device.fd);
        device.fd = -1;
        return device;
    }
    device.node = node_of(device.fd);
    return device;
}

/// libinput normalises as (value - min) / (max - min + 1); the tolerance also
/// admits a plain (max - min), so the test pins the space, not the rounding.
bool near(double got, int32_t raw) { return std::abs(got - raw / 1000.0) < 1e-3; }

/// Run the event loop for `ms`, so libinput gets to read what was injected.
void pump(luminaria::Display& display, unsigned ms) {
    auto stop = display.event_loop().add_timer([&] { display.terminate(); });
    stop.arm(ms);
    display.run();
}

} // namespace

int main() {
    // A "full keyboard" to udev is one that has every key from ESC to S; short
    // of that it is tagged ID_INPUT_KEY only and libinput won't call it a
    // keyboard. Only KEY_LEFTSHIFT is ever pressed.
    std::vector<uint16_t> keyboard_keys;
    for (uint16_t code = KEY_ESC; code <= KEY_S; ++code) {
        keyboard_keys.push_back(code);
    }
    keyboard_keys.push_back(KEY_LEFTSHIFT);

    Uinput keyboard = make_device("luminaria-test-keyboard", keyboard_keys, {});
    // BTN_LEFT is what makes udev call this a mouse; it is declared and never
    // pressed. REL_WHEEL without REL_WHEEL_HI_RES keeps the wheel at exactly
    // one notch per event.
    Uinput mouse = make_device("luminaria-test-mouse", {BTN_LEFT},
                               {REL_X, REL_Y, REL_WHEEL});
    if (keyboard.fd < 0 || mouse.fd < 0 || keyboard.node.empty() || mouse.node.empty()) {
        std::fprintf(stderr, "skip: cannot create uinput devices (%s)\n", std::strerror(errno));
        keyboard.destroy();
        mouse.destroy();
        return 77;
    }
    // libinput opens devices read-write; check for that access up front so the
    // skip says something useful instead of failing inside add_device().
    int probe = open(keyboard.node.c_str(), O_RDWR | O_NONBLOCK);
    if (probe < 0) {
        std::fprintf(stderr,
                     "skip: no access to %s (%s) — needs the 'input' group or root\n",
                     keyboard.node.c_str(), std::strerror(errno));
        keyboard.destroy();
        mouse.destroy();
        return 77;
    }
    close(probe);

    auto display = luminaria::Display::create();
    assert(display.has_value());

    // A udev context picks its own devices off the seat; asking one to take a
    // device by path has to be refused rather than half-work.
    if (auto udev_backend = luminaria::LibinputBackend::create(display->event_loop())) {
        assert(!udev_backend->add_device(keyboard.node));
    }

    auto backend = luminaria::LibinputBackend::create_path(display->event_loop());
    assert(backend.has_value());

    std::vector<luminaria::KeyEvent> keys;
    std::vector<luminaria::ModifiersEvent> mods;
    std::vector<luminaria::PointerMotionEvent> motions;
    std::vector<luminaria::PointerAxisEvent> axes;
    std::vector<luminaria::InputCapabilities> caps;
    // One record per touch event, in order, so the frames between them are checked.
    struct Touch {
        char kind; // d(own) m(otion) u(p) f(rame) c(ancel)
        int32_t id;
        double x, y;
    };
    std::vector<Touch> touches;
    auto on_input = backend->input().connect([&](luminaria::InputEvent& event) {
        if (auto* e = std::get_if<luminaria::KeyEvent>(&event)) {
            keys.push_back(*e);
        } else if (auto* e = std::get_if<luminaria::ModifiersEvent>(&event)) {
            mods.push_back(*e);
        } else if (auto* e = std::get_if<luminaria::PointerMotionEvent>(&event)) {
            motions.push_back(*e);
        } else if (auto* e = std::get_if<luminaria::PointerAxisEvent>(&event)) {
            axes.push_back(*e);
        } else if (auto* e = std::get_if<luminaria::InputCapabilities>(&event)) {
            caps.push_back(*e);
        } else if (auto* e = std::get_if<luminaria::TouchDownEvent>(&event)) {
            touches.push_back({'d', e->id, e->x, e->y});
        } else if (auto* e = std::get_if<luminaria::TouchMotionEvent>(&event)) {
            touches.push_back({'m', e->id, e->x, e->y});
        } else if (auto* e = std::get_if<luminaria::TouchUpEvent>(&event)) {
            touches.push_back({'u', e->id, 0, 0});
        } else if (std::holds_alternative<luminaria::TouchFrameEvent>(event)) {
            touches.push_back({'f', -1, 0, 0});
        } else if (auto* e = std::get_if<luminaria::TouchCancelEvent>(&event)) {
            touches.push_back({'c', e->id, 0, 0});
        }
    });

    if (auto added = backend->add_device(keyboard.node); !added) {
        std::fprintf(stderr, "skip: %s\n", added.error().message.c_str());
        keyboard.destroy();
        mouse.destroy();
        return 77;
    }
    assert(backend->add_device(mouse.node));
    assert(backend->start());

    // --- keyboard: the key itself, and the modifier state it leaves behind ---
    keyboard.emit(EV_KEY, KEY_LEFTSHIFT, 1);
    keyboard.sync();
    pump(*display, 60);
    keyboard.emit(EV_KEY, KEY_LEFTSHIFT, 0);
    keyboard.sync();
    // --- pointer: relative motion and one wheel notch downwards ---
    mouse.emit(EV_REL, REL_X, 5);
    mouse.emit(EV_REL, REL_Y, 3);
    mouse.sync();
    mouse.emit(EV_REL, REL_WHEEL, -1); // evdev: negative is towards the user
    mouse.sync();
    pump(*display, 120);

    assert(keys.size() == 2);
    assert(keys[0].keycode == KEY_LEFTSHIFT && keys[0].pressed);
    assert(keys[1].keycode == KEY_LEFTSHIFT && !keys[1].pressed);
    // Read against the backend's keymap, in the state from before the key:
    // Shift's own press carries no Shift, its release does.
    constexpr uint32_t kShiftL = 0xffe1; // XKB_KEY_Shift_L
    assert(keys[0].keysym == kShiftL && keys[0].modifiers.depressed == 0);
    assert(keys[1].keysym == kShiftL && keys[1].modifiers.depressed != 0);

    // Exactly two: pressing Shift changes the mask, releasing it changes it
    // back, and nothing else in between is worth telling clients about.
    assert(mods.size() == 2);
    assert(mods[0].depressed != 0);
    assert(mods[1].depressed == 0);

    // Pointer acceleration means the deltas are not literally 5 and 3, but the
    // sign and the axis have to survive.
    assert(motions.size() == 1);
    assert(motions[0].dx > 0.0 && motions[0].dy > 0.0);

    assert(axes.size() == 1);
    // Wayland's positive dy is down, which is the opposite sign from evdev's.
    assert(axes[0].dy_steps == 1 && axes[0].dy > 0.0);
    assert(axes[0].dx_steps == 0 && axes[0].dx == 0.0);
    assert(!axes[0].stop_vertical); // a wheel never ends a gesture

    // --- capabilities: both devices, then the mouse goes away ---
    assert(!caps.empty());
    assert(backend->capabilities().keyboard);
    assert(backend->capabilities().pointer);
    assert(!backend->capabilities().touch);

    const size_t caps_before = caps.size();
    mouse.destroy();
    pump(*display, 200);
    assert(caps.size() == caps_before + 1);
    assert(caps.back().keyboard);
    assert(!caps.back().pointer);
    assert(!backend->capabilities().pointer);

    keyboard.destroy();

    // --- touch: two fingers down, one moves, both lift ---
    const char* opt_in = std::getenv("LUMINARIA_TEST_TOUCH");
    if (opt_in == nullptr || std::strcmp(opt_in, "1") != 0) {
        std::fprintf(stderr, "note: touch not exercised; set LUMINARIA_TEST_TOUCH=1 "
                             "(it taps the running desktop's screen)\n");
        return 0;
    }
    Uinput screen = make_touchscreen("luminaria-test-touchscreen");
    if (screen.fd < 0 || screen.node.empty()) {
        std::fprintf(stderr, "skip: cannot create a uinput touchscreen (%s)\n",
                     std::strerror(errno));
        return 77;
    }
    assert(backend->add_device(screen.node));
    pump(*display, 60);
    assert(backend->capabilities().touch);

    screen.emit(EV_ABS, ABS_MT_SLOT, 0);
    screen.emit(EV_ABS, ABS_MT_TRACKING_ID, 1);
    screen.emit(EV_ABS, ABS_MT_POSITION_X, 250);
    screen.emit(EV_ABS, ABS_MT_POSITION_Y, 500);
    screen.emit(EV_KEY, BTN_TOUCH, 1);
    screen.emit(EV_ABS, ABS_X, 250);
    screen.emit(EV_ABS, ABS_Y, 500);
    screen.sync();
    screen.emit(EV_ABS, ABS_MT_SLOT, 1);
    screen.emit(EV_ABS, ABS_MT_TRACKING_ID, 2);
    screen.emit(EV_ABS, ABS_MT_POSITION_X, 750);
    screen.emit(EV_ABS, ABS_MT_POSITION_Y, 250);
    screen.sync();
    screen.emit(EV_ABS, ABS_MT_SLOT, 0);
    screen.emit(EV_ABS, ABS_MT_POSITION_X, 300);
    screen.emit(EV_ABS, ABS_X, 300);
    screen.sync();
    screen.emit(EV_ABS, ABS_MT_SLOT, 1);
    screen.emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    screen.sync();
    screen.emit(EV_ABS, ABS_MT_SLOT, 0);
    screen.emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    screen.emit(EV_KEY, BTN_TOUCH, 0);
    screen.sync();
    pump(*display, 120);

    assert(touches.size() == 10);
    // Each sample is its own frame, so every event is followed by one.
    for (size_t i = 1; i < touches.size(); i += 2) {
        assert(touches[i].kind == 'f');
    }
    // The ids are seat slots: the first two free ones.
    assert(touches[0].kind == 'd' && touches[0].id == 0);
    assert(near(touches[0].x, 250) && near(touches[0].y, 500));
    assert(touches[2].kind == 'd' && touches[2].id == 1);
    assert(near(touches[2].x, 750) && near(touches[2].y, 250));
    // Only the axis that changed moved; the other is carried over.
    assert(touches[4].kind == 'm' && touches[4].id == 0);
    assert(near(touches[4].x, 300) && near(touches[4].y, 500));
    assert(touches[6].kind == 'u' && touches[6].id == 1);
    assert(touches[8].kind == 'u' && touches[8].id == 0);

    screen.destroy();
    return 0;
}
