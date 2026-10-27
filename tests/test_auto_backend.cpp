#include <typeinfo>
// AutoBackend: the backend pick every compositor used to copy. What this guards
// is the two picks a machine without a display can make — headless asked for by
// name, and auto falling all the way through to it — plus the name parser.
// Nested and DRM need a parent compositor and a free VT; the examples run those.
#include <cassert>
#include <cstdio>
#include <cstdlib>

import luminaria.shell;
import std;

namespace lm = luminaria;

namespace {

// One output arrives at start(), the size asked for, and nothing else is there.
void check_headless(lm::AutoBackend& picked, int width, int height) {
    assert(picked.kind() == lm::BackendKind::headless);
    assert(picked.input() == nullptr);
    assert(picked.nested() == nullptr);
    assert(picked.drm() == nullptr);
    assert(picked.libinput() == nullptr);
    assert(picked.session() == nullptr);
    assert(picked.session_error().empty());
    assert(!picked.vulkan_options().drm_device.has_value());

    std::vector<lm::Output*> outputs;
    auto conn = picked.backend().new_output.connect(
        [&](lm::NewOutput& e) { outputs.push_back(&e.output); });
    assert(outputs.empty()); // nothing before start()
    auto started = picked.start();
    assert(started.has_value());
    assert(outputs.size() == 1);
    assert(outputs[0]->width() == width && outputs[0]->height() == height);
}

} // namespace

int main() {
    for (const auto* name : {"auto", "nested", "drm", "headless"}) {
        auto kind = lm::parse_backend_kind(name);
        assert(kind.has_value());
        assert(lm::backend_kind_name(*kind) == name);
    }
    assert(*lm::parse_backend_kind("auto") == lm::BackendKind::automatic);
    assert(!lm::parse_backend_kind("Headless").has_value());
    assert(!lm::parse_backend_kind("").has_value());

    // Headless by name: whatever else the machine has, it is not tried.
    {
        auto display = lm::Display::create();
        assert(display.has_value());
        auto picked = lm::AutoBackend::create(
            display->event_loop(), {.kind = lm::BackendKind::headless, .width = 640, .height = 480});
        assert(picked.has_value());
        check_headless(*picked, 640, 480);
    }

    // Auto with no parent compositor and no seat. libwayland falls back to
    // wayland-0 without WAYLAND_DISPLAY, so the runtime dir is an empty one;
    // libseat fails outright on a backend name it does not have.
    char runtime[] = "/tmp/luminaria-auto-backend-XXXXXX";
    assert(mkdtemp(runtime) != nullptr);
    setenv("XDG_RUNTIME_DIR", runtime, 1);
    unsetenv("WAYLAND_DISPLAY");
    unsetenv("WAYLAND_SOCKET");
    setenv("LIBSEAT_BACKEND", "luminaria-test-none", 1);
    int status = 0;
    {
        auto display = lm::Display::create();
        assert(display.has_value());
        if (auto session = lm::Session::create(display->event_loop()); session) {
            std::fprintf(stderr, "skip: a libseat session opened anyway; auto would try DRM\n");
            status = 77;
        } else {
            auto picked = lm::AutoBackend::create(display->event_loop());
            assert(picked.has_value());
            check_headless(*picked, 1280, 800);
        }
    }
    // Asked for by name, the same machine fails instead of falling back.
    {
        auto display = lm::Display::create();
        assert(display.has_value());
        assert(!lm::AutoBackend::create(display->event_loop(), {.kind = lm::BackendKind::nested})
                    .has_value());
    }
    std::filesystem::remove(runtime);
    return status;
}
