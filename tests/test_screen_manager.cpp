// ScreenManager: an output from arrival to removal, given only a scene builder.
//
// Nothing below connects to `frame`, `present` or `destroy`, calls `reset()` or
// `present()`, opens an OutputGlobal or answers the shell's popup query. That is
// the test: every one of those was a contract a compositor had to remember, so a
// client that gets through all of the following proves they are the library's.
//
//   1. outputs arrive into the layout, with a wl_output named by the options;
//   2. a client's window is drawn, and its frame callbacks are released — also
//      for a frame that turns out `unchanged`, when no `present` follows;
//   3. a popup at the bottom edge flips, with no constraint query installed, and
//      its map asks for a frame;
//   4. a monitor unplugged takes all of its state with it, and the number in a
//      name is not reused;
//   5. a steady frame allocates nothing, a menu open and a rectangle moving;
//   6. the options that turn things off do.
//
// Run twice, on the machine's GPU when there is one and then with Vulkan made
// unavailable, so the CPU path is covered on every machine.
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

#include "xdg-shell-client-protocol.h"

import luminaria.shell;
import std;

#include "shell_harness.h"

namespace {

namespace lm = luminaria;
using harness::Client;
using harness::ManualBackend;
using harness::ManualOutput;
using harness::settle;

constexpr int kW = 64, kH = 48;

std::atomic<std::uint64_t> g_allocs{0};

void whole_life(lm::SceneRenderer& renderer) {
    auto display = lm::must(lm::Display::create(), "display");
    auto globals = lm::must(lm::CoreGlobals::create(display), "globals");
    lm::XdgShell& shell = globals.shell();
    lm::OutputLayout layout;
    lm::PopupTree popups(shell);
    ManualBackend backend;
    ManualOutput& first = backend.add(kW, kH);
    ManualOutput* second = &backend.add(32, kH);

    std::vector<lm::Toplevel*> stack;
    std::vector<lm::SceneItem> scene;
    bool covered = false;
    int moved = -1;
    int built = 0;
    lm::ScreenManager screens(display, backend, renderer, layout,
                              {.background = {0.2f, 0.2f, 0.2f, 1.0f}, .name = "t"});
    screens.follow_popups(popups);
    screens.set_scene_builder([&](lm::Output&) -> std::span<const lm::SceneItem> {
        ++built;
        scene.clear();
        for (lm::Toplevel* t : stack) {
            scene.push_back(lm::toplevel_item(*t, 0, 0));
        }
        popups.append_to(scene);
        if (covered) {
            scene.push_back({.kind = lm::SceneItem::Kind::rect,
                             .box = {0, 0, kW, kH},
                             .color = {1.0f, 0.0f, 0.0f, 1.0f}});
        }
        if (moved >= 0) {
            scene.push_back({.kind = lm::SceneItem::Kind::rect,
                             .box = {moved, 0, 4, 4},
                             .color = {0.0f, 0.0f, 1.0f, 1.0f}});
        }
        return scene;
    });

    lm::Connections connections;
    connections += shell.new_toplevel().connect([&](lm::NewToplevel& e) {
        lm::Toplevel* t = &e.toplevel;
        connections += t->map.connect([&, t] {
            stack.push_back(t);
            screens.invalidate();
        });
        connections += t->destroy.connect([&, t] {
            std::erase(stack, t);
            screens.invalidate();
        });
    });

    // 1. Arrival: placed left to right, asked for a frame, announced.
    assert(screens.size() == 0);
    assert(backend.start());
    assert(screens.size() == 2);
    assert(layout.box_of(first) == (lm::Box{0, 0, kW, kH}));
    assert(layout.box_of(*second) == (lm::Box{kW, 0, 32, kH}));
    assert(first.frame_scheduled() && second->frame_scheduled());

    Client client(display);
    assert(client.output_globals.size() == 2);
    settle(client, first);
    settle(client, *second);
    assert(built >= 2);
    assert(first.flips >= 1 && second->flips >= 1);
    assert(client.output_names.size() == 2);
    assert(client.output_names[0] == "t-1" && client.output_names[1] == "t-2");

    // 2. A window: drawn, and its callbacks released without a handler anywhere.
    const Client::Window window = client.window(kW, kH);
    assert(stack.size() == 1);
    assert(first.frame_scheduled());
    client.frame_callback(window.surface);
    wl_surface_commit(window.surface);
    settle(client, first);
    assert(client.frames_done == 1);
    assert(screens.surface_box(stack[0]->surface().id()) == (lm::Box{0, 0, kW, kH}));
    assert(!screens.surface_box(lm::SurfaceId{}).has_value());
    assert(screens.scene_at(1.0, 1.0).size() == 1);
    assert(screens.scene_at(kW + 1.0, 1.0).size() == 1); // the second output's list

    // Covered, the same commit changes nothing on screen: on the GPU that is an
    // `unchanged` frame, for which no `present` will ever come.
    covered = true;
    screens.invalidate();
    settle(client, first);
    const lm::SceneStats before = renderer.stats();
    const int flips = first.flips;
    client.frame_callback(window.surface);
    wl_surface_commit(window.surface);
    settle(client, first);
    assert(client.frames_done == 2);
    if (renderer.gpu_enabled()) {
        assert(renderer.stats().unchanged > before.unchanged);
        assert(first.flips == flips);
    }
    covered = false;
    screens.invalidate();
    settle(client, first);

    // 3. A menu at the bottom edge flips, and nobody told the shell anything.
    settle(client, first);
    assert(!first.frame_scheduled());
    const Client::Window menu = client.popup(window, {10, 40, 20, 8}, 20, 20);
    assert(client.popup_y == 20); // above the anchor; under it would be off-screen
    assert(first.frame_scheduled());
    assert(scene.empty() || !stack.empty());
    settle(client, first);
    assert(screens.surface_box(stack[0]->surface().id()).has_value());
    assert(screens.scene_at(1.0, 1.0).size() == 2);

    // 4. Unplugged: out of the layout, out of the registry, out of the manager.
    backend.remove(*second);
    second = nullptr;
    assert(screens.size() == 1);
    assert(layout.outputs().size() == 1);
    client.pump();
    assert(client.output_globals.size() == 1);
    ManualOutput& replugged = backend.add(32, kH);
    assert(screens.size() == 2);
    settle(client, replugged);
    assert(client.output_names.back() == "t-3");

    // 5. A steady frame allocates nothing, with something actually changing.
    moved = 0;
    for (int i = 0; i < 12; ++i) { // the scratch reaches its size
        moved = i % 8;
        first.schedule_frame();
        first.fire();
        first.fire();
        client.pump();
    }
    const std::uint64_t allocs = g_allocs.load();
    for (int i = 0; i < 32; ++i) {
        moved = i % 8;
        first.schedule_frame();
        first.fire();
        first.fire();
    }
    const std::uint64_t spent = g_allocs.load() - allocs;
    if (spent != 0) {
        std::println(std::cerr, "{} heap allocations in 32 frames", spent);
    }
    assert(spent == 0);
    moved = -1;

    xdg_popup_destroy(menu.popup);
    xdg_surface_destroy(menu.xdg);
    wl_surface_destroy(menu.surface);
    client.pump();
}

/// Everything that can be turned off, off.
void options_respected(lm::SceneRenderer& renderer) {
    auto display = lm::must(lm::Display::create(), "display");
    auto globals = lm::must(lm::CoreGlobals::create(display), "globals");
    lm::OutputLayout layout;
    ManualBackend backend;
    ManualOutput& output = backend.add(kW, kH);
    lm::ScreenManager screens(display, backend, renderer, layout,
                              {.join_layout = false, .announce = false});
    assert(backend.start());
    assert(screens.size() == 1);
    assert(layout.empty());
    Client client(display);
    assert(client.output_globals.empty());
    // Not in the layout, but still drawn: from the origin.
    settle(client, output);
    assert(output.flips >= 1);
    // No layout to look in, and still a list to route against.
    std::vector<lm::SceneItem> scene{{.kind = lm::SceneItem::Kind::rect, .box = {0, 0, 4, 4}}};
    screens.set_scene_builder([&](lm::Output&) { return std::span<const lm::SceneItem>{scene}; });
    assert(screens.scene_at(1.0, 1.0).size() == 1);
}

} // namespace

namespace {
void* counted_alloc(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n != 0 ? n : 1);
}
} // namespace

// All of it, or under a sanitizer the pair ends up crossed: see tests/test_frame.cpp.
void* operator new(std::size_t n) {
    void* p = counted_alloc(n);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
void* operator new[](std::size_t n) {
    void* p = counted_alloc(n);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

int main() {
    // The layer allocates through the replaced operator new.
    unsetenv("LUMINARIA_VK_VALIDATION");
    {
        lm::SceneRenderer renderer;
        std::println("{} path", renderer.gpu_enabled() ? "GPU" : "CPU");
        whole_life(renderer);
        options_respected(renderer);
    }
    setenv("VK_ICD_FILENAMES", "/nonexistent/luminaria-test-icd.json", 1);
    setenv("VK_DRIVER_FILES", "/nonexistent/luminaria-test-icd.json", 1);
    {
        lm::SceneRenderer renderer;
        assert(!renderer.gpu_enabled());
        whole_life(renderer);
        options_respected(renderer);
    }
    return 0;
}
