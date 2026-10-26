// luminaria_rs.cpp — the C++ half of the Rust binding, and the only place the
// modules appear. xmake compiles it (clang + C++ modules); cc never sees it.
//
// This is an ordinary translation unit that imports a module, not a module
// unit, so it has no `module;` line: includes first, then the import, exactly
// like examples/minimal.cpp.
//
// What it is: one thin shim per library type, declared in luminaria_rs.h.
// Each holds the luminaria object (or a pointer to one it does not own) and
// turns its `Result`s into a bool plus an error string, and its signals into
// queues Rust drains. No policy lives here; that is the compositor's, in Rust.
//
// The rule that keeps it sound: C++ never calls Rust. libwayland's callbacks
// only append to a shim's queue, and Rust drains it after `dispatch()` has
// returned, while no luminaria signal is on the stack.
#include <unistd.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "luminaria/luminaria_rs.h"
#include "luminaria/src/lib.rs.h"

import luminaria.gpu;
import luminaria.desktop;

namespace lum {

namespace lm = luminaria;

template <class E>
using Conn = typename lm::Signal<E>::Connection;

namespace {

// Client strings are not guaranteed UTF-8, and rust::String's checked
// constructor throws on bad input. Lossy never throws.
rust::String to_rust(const std::string& s) { return rust::String::lossy(s); }

} // namespace

// ============================================================================
// Core & Util
// ============================================================================

struct DisplayShim::Impl {
    std::optional<lm::Display> display;
};

DisplayShim::~DisplayShim() = default;
DisplayShim::DisplayShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DisplayShim::Impl& DisplayShim::impl() const { return *impl_; }

std::unique_ptr<DisplayShim> display_new(rust::String& error) {
    try {
        auto r = lm::Display::create();
        if (!r) {
            error = to_rust(r.error().message);
            return nullptr;
        }
        auto impl = std::make_unique<DisplayShim::Impl>();
        impl->display.emplace(std::move(*r));
        return std::make_unique<DisplayShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

int DisplayShim::fd() const {
    if (!impl_->display) return -1;
    return wl_event_loop_get_fd(impl_->display->event_loop().c_ptr());
}

rust::String DisplayShim::add_socket_auto(rust::String& error) const {
    if (!impl_->display) { error = "display not initialized"; return ""; }
    auto r = impl_->display->add_socket_auto();
    if (!r) {
        error = to_rust(r.error().message);
        return "";
    }
    return to_rust(*r);
}

bool DisplayShim::add_socket(rust::Str name, rust::String& error) const {
    if (!impl_->display) { error = "display not initialized"; return false; }
    int ret = wl_display_add_socket(impl_->display->c_ptr(), std::string(name).c_str());
    if (ret != 0) {
        error = "wl_display_add_socket failed";
        return false;
    }
    return true;
}

bool DisplayShim::init_shm(rust::String& error) const {
    if (!impl_->display) { error = "display not initialized"; return false; }
    auto st = impl_->display->init_shm();
    if (!st) {
        error = to_rust(st.error().message);
        return false;
    }
    return true;
}

void DisplayShim::run() const {
    if (impl_->display) impl_->display->run();
}

void DisplayShim::terminate() const {
    if (impl_->display) impl_->display->terminate();
}

int DisplayShim::dispatch(int timeout_ms) const {
    if (!impl_->display) return -1;
    wl_display_flush_clients(impl_->display->c_ptr());
    return wl_event_loop_dispatch(impl_->display->event_loop().c_ptr(), timeout_ms);
}

void DisplayShim::flush_clients() const {
    if (impl_->display) wl_display_flush_clients(impl_->display->c_ptr());
}

std::size_t DisplayShim::raw_display() const {
    return impl_->display ? reinterpret_cast<std::size_t>(impl_->display->c_ptr()) : 0;
}

struct KeymapStateShim::Impl {
    std::optional<lm::KeymapState> state;
};

KeymapStateShim::~KeymapStateShim() = default;
KeymapStateShim::KeymapStateShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
KeymapStateShim::Impl& KeymapStateShim::impl() const { return *impl_; }

std::unique_ptr<KeymapStateShim> keymap_state_from_layout(rust::Str layout, rust::String& error) {
    try {
        std::string layout_str(layout);
        auto r = lm::KeymapState::from_layout(layout_str);
        if (!r) {
            error = to_rust(r.error().message);
            return nullptr;
        }
        auto impl = std::make_unique<KeymapStateShim::Impl>();
        impl->state.emplace(std::move(*r));
        return std::make_unique<KeymapStateShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::unique_ptr<KeymapStateShim> keymap_state_from_text(rust::Str text, rust::String& error) {
    try {
        std::string text_str(text);
        auto r = lm::KeymapState::from_text(text_str);
        if (!r) {
            error = to_rust(r.error().message);
            return nullptr;
        }
        auto impl = std::make_unique<KeymapStateShim::Impl>();
        impl->state.emplace(std::move(*r));
        return std::make_unique<KeymapStateShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

rust::String KeymapStateShim::text() const {
    if (!impl_->state) return "";
    return to_rust(impl_->state->text());
}

std::uint32_t KeymapStateShim::key_keysym(std::uint32_t code) const {
    if (!impl_->state) return 0;
    return impl_->state->keysym(code);
}

void KeymapStateShim::update_key(std::uint32_t code, bool pressed) {
    if (impl_->state) impl_->state->update_key(code, pressed);
}

std::uint32_t KeymapStateShim::modifiers_depressed() const {
    if (!impl_->state) return 0;
    return impl_->state->modifiers().depressed;
}

std::uint32_t KeymapStateShim::modifiers_latched() const {
    if (!impl_->state) return 0;
    return impl_->state->modifiers().latched;
}

std::uint32_t KeymapStateShim::modifiers_locked() const {
    if (!impl_->state) return 0;
    return impl_->state->modifiers().locked;
}

std::uint32_t KeymapStateShim::modifiers_group() const {
    if (!impl_->state) return 0;
    return impl_->state->modifiers().group;
}

struct RegionShim::Impl {
    lm::Region region;
};

RegionShim::~RegionShim() = default;
RegionShim::RegionShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RegionShim::Impl& RegionShim::impl() const { return *impl_; }

std::unique_ptr<RegionShim> region_new() {
    return std::make_unique<RegionShim>(std::make_unique<RegionShim::Impl>());
}

std::unique_ptr<RegionShim> region_from_box(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    auto impl = std::make_unique<RegionShim::Impl>();
    impl->region = lm::Region{lm::Box{x, y, width, height}};
    return std::make_unique<RegionShim>(std::move(impl));
}

void RegionShim::add(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    impl_->region.add(lm::Box{x, y, width, height});
}

void RegionShim::subtract_box(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    impl_->region.subtract(lm::Box{x, y, width, height});
}

void RegionShim::subtract_region(const RegionShim& other) {
    impl_->region.subtract(other.impl_->region);
}

void RegionShim::intersect_box(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    impl_->region.intersect(lm::Box{x, y, width, height});
}

void RegionShim::intersect_region(const RegionShim& other) {
    lm::Region diff = impl_->region;
    diff.subtract(other.impl_->region);
    impl_->region.subtract(diff);
}

bool RegionShim::empty() const {
    return impl_->region.empty();
}

void RegionShim::clear() {
    impl_->region.clear();
}

bool RegionShim::contains(std::int32_t x, std::int32_t y) const {
    return impl_->region.contains(x, y);
}

Rect RegionShim::extents() const {
    const auto b = impl_->region.extents();
    return Rect{b.x, b.y, b.width, b.height};
}

rust::Vec<Rect> RegionShim::rects() const {
    rust::Vec<Rect> out;
    for (const auto& b : impl_->region.rects()) {
        out.push_back(Rect{b.x, b.y, b.width, b.height});
    }
    return out;
}

bool layout_fits_raw(std::int32_t width, std::int32_t height, std::uint32_t stride,
                     std::uint32_t offset, std::size_t total_size) {
    if (!lm::layout_fits(width, height, stride, offset)) return false;
    return lm::layout_length(width, height, stride, offset) <= total_size;
}

// ============================================================================
// Backend & Output
// ============================================================================

struct OutputShim::Impl {
    lm::Output* output = nullptr;
    // Identity that outlives the output, so Rust can match a removal to the
    // Output it holds. Never reused.
    std::uint64_t id = 0;
    bool frame_pending = false;
    bool mode_changed = false;
    Conn<lm::OutputDestroy> on_destroy;
    Conn<lm::FrameEvent> on_frame;
    Conn<lm::OutputModeChange> on_mode;
};

OutputShim::~OutputShim() = default;
OutputShim::OutputShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
OutputShim::Impl& OutputShim::impl() const { return *impl_; }

std::int32_t OutputShim::width() const {
    return impl_->output ? impl_->output->width() : 0;
}
std::int32_t OutputShim::height() const {
    return impl_->output ? impl_->output->height() : 0;
}
std::int32_t OutputShim::scale() const {
    return impl_->output ? impl_->output->scale() : 1;
}
void OutputShim::set_scale(std::int32_t scale) {
    if (impl_->output) impl_->output->set_scale(scale);
}
std::uint32_t OutputShim::transform() const {
    return impl_->output ? static_cast<std::uint32_t>(impl_->output->transform()) : 0;
}
void OutputShim::set_transform(std::uint32_t transform) {
    if (impl_->output) impl_->output->set_transform(static_cast<lm::Transform>(transform));
}
std::int32_t OutputShim::logical_width() const {
    return impl_->output ? impl_->output->logical_width() : 0;
}
std::int32_t OutputShim::logical_height() const {
    return impl_->output ? impl_->output->logical_height() : 0;
}
void OutputShim::schedule_frame() {
    if (impl_->output) impl_->output->schedule_frame();
}
bool OutputShim::frame_scheduled() const {
    return impl_->output ? impl_->output->frame_scheduled() : false;
}

rust::Vec<Mode> OutputShim::modes() const {
    rust::Vec<Mode> out;
    if (!impl_->output) return out;
    for (const auto& m : impl_->output->modes()) {
        out.push_back(Mode{m.width, m.height, m.refresh_mhz, m.preferred});
    }
    return out;
}

Mode OutputShim::current_mode() const {
    if (!impl_->output) return Mode{0, 0, 0, false};
    const auto m = impl_->output->current_mode();
    return Mode{m.width, m.height, m.refresh_mhz, m.preferred};
}

bool OutputShim::set_mode(std::int32_t width, std::int32_t height, std::int32_t refresh_mhz, rust::String& error) {
    if (!impl_->output) { error = "output destroyed"; return false; }
    auto st = impl_->output->set_mode(width, height, refresh_mhz);
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

bool OutputShim::commit(float r, float g, float b, float a, rust::String& error) {
    if (!impl_->output) { error = "output destroyed"; return false; }
    auto st = impl_->output->commit(lm::Color{r, g, b, a});
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

bool OutputShim::commit_frame(rust::Slice<const std::uint8_t> rgba, std::int32_t width, std::int32_t height, rust::String& error) {
    if (!impl_->output) { error = "output destroyed"; return false; }
    const auto* ptr = reinterpret_cast<const lm::Pixel*>(rgba.data());
    std::span<const lm::Pixel> span(ptr, rgba.size() / sizeof(lm::Pixel));
    auto st = impl_->output->commit_frame(span, width, height);
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

rust::Vec<std::uint64_t> OutputShim::scanout_modifiers(std::uint32_t drm_format) const {
    rust::Vec<std::uint64_t> out;
    if (!impl_->output) return out;
    for (auto m : impl_->output->scanout_modifiers(drm_format)) {
        out.push_back(m);
    }
    return out;
}

bool OutputShim::has_cursor_plane() const {
    return impl_->output ? impl_->output->has_cursor_plane() : false;
}

bool OutputShim::move_cursor(std::int32_t x, std::int32_t y, rust::String& error) {
    if (!impl_->output) { error = "output destroyed"; return false; }
    auto st = impl_->output->move_cursor(x, y);
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

bool OutputShim::hide_cursor(rust::String& error) {
    if (!impl_->output) { error = "output destroyed"; return false; }
    auto st = impl_->output->hide_cursor();
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

bool OutputShim::take_frame() {
    return std::exchange(impl_->frame_pending, false);
}

bool OutputShim::take_mode_changed() {
    return std::exchange(impl_->mode_changed, false);
}

bool OutputShim::alive() const { return impl_->output != nullptr; }

std::uint64_t OutputShim::id() const { return impl_->id; }

inline std::unique_ptr<OutputShim> make_output_shim(lm::Output* output) {
    static std::atomic<std::uint64_t> next_id{1};
    if (!output) return nullptr;
    auto impl = std::make_unique<OutputShim::Impl>();
    impl->output = output;
    impl->id = next_id.fetch_add(1, std::memory_order_relaxed);
    impl->on_mode = output->mode_changed.connect([impl = impl.get()](lm::OutputModeChange&) {
        impl->mode_changed = true;
    });
    impl->on_destroy = output->destroy.connect([impl = impl.get()](lm::OutputDestroy&) {
        impl->output = nullptr;
    });
    impl->on_frame = output->frame.connect([impl = impl.get()](lm::FrameEvent&) {
        impl->frame_pending = true;
    });
    return std::make_unique<OutputShim>(std::move(impl));
}

struct HeadlessBackendShim::Impl {
    std::unique_ptr<lm::HeadlessBackend> backend;
};

HeadlessBackendShim::~HeadlessBackendShim() = default;
HeadlessBackendShim::HeadlessBackendShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
HeadlessBackendShim::Impl& HeadlessBackendShim::impl() const { return *impl_; }

std::unique_ptr<HeadlessBackendShim> headless_backend_new(const DisplayShim& display, unsigned int interval_ms, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto backend = std::make_unique<lm::HeadlessBackend>(display.impl().display->event_loop(), interval_ms);
        auto impl = std::make_unique<HeadlessBackendShim::Impl>();
        impl->backend = std::move(backend);
        return std::make_unique<HeadlessBackendShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::unique_ptr<OutputShim> HeadlessBackendShim::add_output(std::int32_t width, std::int32_t height) {
    if (!impl_->backend) return nullptr;
    lm::Output& out = impl_->backend->add_output(width, height);
    return make_output_shim(&out);
}

bool HeadlessBackendShim::start(rust::String& error) {
    if (!impl_->backend) { error = "backend not initialized"; return false; }
    auto st = impl_->backend->start();
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

struct WaylandBackendShim::Impl {
    std::unique_ptr<lm::WaylandBackend> backend;
    std::deque<BackendInputEventRaw> input_events;
    Conn<lm::KeyEvent> on_key;
    Conn<lm::PointerMotionAbsEvent> on_motion;
    Conn<lm::PointerButtonEvent> on_button;
};

WaylandBackendShim::~WaylandBackendShim() = default;
WaylandBackendShim::WaylandBackendShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
WaylandBackendShim::Impl& WaylandBackendShim::impl() const { return *impl_; }

std::unique_ptr<WaylandBackendShim> wayland_backend_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::WaylandBackend::create(display.impl().display->event_loop());
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<WaylandBackendShim::Impl>();
        impl->backend = std::make_unique<lm::WaylandBackend>(std::move(*r));
        auto* raw_impl = impl.get();
        raw_impl->on_key = raw_impl->backend->key.connect([raw_impl](lm::KeyEvent& e) {
            raw_impl->input_events.push_back(BackendInputEventRaw{0 /* key */, e.keycode, e.pressed, 0.0, 0.0});
        });
        raw_impl->on_motion = raw_impl->backend->pointer_motion.connect([raw_impl](lm::PointerMotionAbsEvent& e) {
            raw_impl->input_events.push_back(BackendInputEventRaw{1 /* motion */, 0, false, e.x, e.y});
        });
        raw_impl->on_button = raw_impl->backend->pointer_button.connect([raw_impl](lm::PointerButtonEvent& e) {
            raw_impl->input_events.push_back(BackendInputEventRaw{2 /* button */, e.button, e.pressed, 0.0, 0.0});
        });
        return std::make_unique<WaylandBackendShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::unique_ptr<OutputShim> WaylandBackendShim::add_output(std::int32_t width, std::int32_t height) {
    if (!impl_->backend) return nullptr;
    lm::Output& out = impl_->backend->add_output(width, height);
    return make_output_shim(&out);
}

bool WaylandBackendShim::start(rust::String& error) {
    if (!impl_->backend) { error = "backend not initialized"; return false; }
    auto st = impl_->backend->start();
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

rust::String WaylandBackendShim::keymap() const {
    if (!impl_->backend) return "";
    return to_rust(impl_->backend->keymap());
}

bool WaylandBackendShim::next_input_event(BackendInputEventRaw& out) {
    if (impl_->input_events.empty()) return false;
    out = impl_->input_events.front();
    impl_->input_events.pop_front();
    return true;
}

struct SessionShim::Impl {
    std::unique_ptr<lm::Session> session;
    std::deque<bool> activity;
    Conn<lm::SessionActive> on_activity;
};

SessionShim::~SessionShim() = default;
SessionShim::SessionShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SessionShim::Impl& SessionShim::impl() const { return *impl_; }

std::unique_ptr<SessionShim> session_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::Session::create(display.impl().display->event_loop());
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<SessionShim::Impl>();
        impl->session = std::make_unique<lm::Session>(std::move(*r));
        impl->on_activity = impl->session->activity().connect(
            [raw = impl.get()](lm::SessionActive& e) { raw->activity.push_back(e.active); });
        return std::make_unique<SessionShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

bool SessionShim::switch_to(int vt, rust::String& error) const {
    if (!impl_->session) { error = "session not initialized"; return false; }
    auto st = impl_->session->switch_to(vt);
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

bool SessionShim::active() const {
    return impl_->session ? impl_->session->active() : false;
}

rust::String SessionShim::seat_name() const {
    return "seat0";
}

std::uint32_t SessionShim::next_activity() const {
    if (impl_->activity.empty()) return 0;
    const bool active = impl_->activity.front();
    impl_->activity.pop_front();
    return active ? 2 : 1;
}

struct DrmBackendShim::Impl {
    struct Event {
        std::unique_ptr<OutputShim> added; // null: a removal
        std::uint64_t removed = 0;
    };
    // Hotplug watch on each output the backend reported, keyed by the shim's id.
    struct Watch {
        std::uint64_t id;
        Conn<lm::OutputDestroy> on_destroy;
    };

    std::unique_ptr<lm::DrmBackend> backend;
    std::deque<Event> events;
    std::deque<Watch> watches;
    Conn<lm::NewOutput> on_new_output;

    // Every output, the ones start() finds included, reaches Rust as an event:
    // C++ never calls Rust, so a signal can only leave something to be drained.
    void watch() {
        on_new_output = backend->new_output.connect([this](lm::NewOutput& e) {
            auto shim = make_output_shim(&e.output);
            const std::uint64_t id = shim->id();
            Watch& w = watches.emplace_back();
            w.id = id;
            w.on_destroy = e.output.destroy.connect([this, id](lm::OutputDestroy&) {
                events.push_back(Event{nullptr, id});
                // Erasing the watch inside its own slot is safe: a Signal never
                // frees the slot that is running.
                std::erase_if(watches, [id](const Watch& x) { return x.id == id; });
            });
            events.push_back(Event{std::move(shim), 0});
        });
    }
};

DrmBackendShim::~DrmBackendShim() = default;
DrmBackendShim::DrmBackendShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DrmBackendShim::Impl& DrmBackendShim::impl() const { return *impl_; }

std::unique_ptr<DrmBackendShim> drm_backend_new(const DisplayShim& display, rust::Str device, const SessionShim& session, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto loop = display.impl().display->event_loop();
        lm::Session* sess = session.impl().session.get();
        lm::Result<lm::DrmBackend> r = device.empty()
            ? lm::DrmBackend::create(loop, sess)
            : lm::DrmBackend::create(loop, std::string(device), sess);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<DrmBackendShim::Impl>();
        impl->backend = std::make_unique<lm::DrmBackend>(std::move(*r));
        impl->watch();
        return std::make_unique<DrmBackendShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::unique_ptr<DrmBackendShim> drm_backend_new_standalone(const DisplayShim& display, rust::Str device, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto loop = display.impl().display->event_loop();
        lm::Result<lm::DrmBackend> r = device.empty()
            ? lm::DrmBackend::create(loop, nullptr)
            : lm::DrmBackend::create(loop, std::string(device), nullptr);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<DrmBackendShim::Impl>();
        impl->backend = std::make_unique<lm::DrmBackend>(std::move(*r));
        impl->watch();
        return std::make_unique<DrmBackendShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

bool DrmBackendShim::start(rust::String& error) {
    if (!impl_->backend) { error = "backend not initialized"; return false; }
    auto st = impl_->backend->start();
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

int DrmBackendShim::fd() const {
    return impl_->backend ? impl_->backend->fd() : -1;
}

void DrmBackendShim::scan_connectors() {
    if (impl_->backend) impl_->backend->scan_connectors();
}

std::unique_ptr<OutputShim> DrmBackendShim::next_event(std::uint32_t& kind, std::uint64_t& removed) {
    kind = 0;
    if (impl_->events.empty()) return nullptr;
    Impl::Event e = std::move(impl_->events.front());
    impl_->events.pop_front();
    if (e.added) {
        kind = 1;
        return std::move(e.added);
    }
    kind = 2;
    removed = e.removed;
    return nullptr;
}

// Event kinds, matched by src/backend.rs.
enum : std::uint32_t {
    kLiKey, kLiModifiers, kLiMotion, kLiButton, kLiAxis,
    kLiTouchDown, kLiTouchMotion, kLiTouchUp, kLiTouchFrame, kLiTouchCancel,
    kLiCapabilities,
};

struct LibinputBackendShim::Impl {
    std::unique_ptr<lm::LibinputBackend> backend;
    std::deque<LibinputEventRaw> events;
    Conn<lm::KeyEvent> on_key;
    Conn<lm::ModifiersEvent> on_modifiers;
    Conn<lm::PointerMotionEvent> on_motion;
    Conn<lm::PointerButtonEvent> on_button;
    Conn<lm::PointerAxisEvent> on_axis;
    Conn<lm::TouchDownEvent> on_touch_down;
    Conn<lm::TouchMotionEvent> on_touch_motion;
    Conn<lm::TouchUpEvent> on_touch_up;
    Conn<lm::TouchFrameEvent> on_touch_frame;
    Conn<lm::TouchCancelEvent> on_touch_cancel;
    Conn<lm::InputCapabilities> on_caps;

    void push(std::uint32_t kind, auto&& fill) {
        LibinputEventRaw ev{};
        ev.kind = kind;
        fill(ev);
        events.push_back(ev);
    }

    void watch() {
        lm::LibinputBackend& b = *backend;
        // The key signal fires before the backend's xkb state takes the key in,
        // and Rust reads the queue only after dispatch() returns, by which time
        // later keys of the same batch have moved the state on. So the keysym
        // and masks a binding is matched against are taken here, as C++ would
        // read them in its handler.
        on_key = b.key().connect([this](lm::KeyEvent& e) {
            push(kLiKey, [&](LibinputEventRaw& ev) {
                const lm::KeymapState& ks = backend->keymap_state();
                const lm::ModifiersEvent m = ks.modifiers();
                ev.code = e.keycode;
                ev.pressed = e.pressed;
                ev.keysym = ks.keysym(e.keycode);
                ev.depressed = m.depressed;
                ev.latched = m.latched;
                ev.locked = m.locked;
                ev.group = m.group;
            });
        });
        on_modifiers = b.modifiers().connect([this](lm::ModifiersEvent& e) {
            push(kLiModifiers, [&](LibinputEventRaw& ev) {
                ev.depressed = e.depressed;
                ev.latched = e.latched;
                ev.locked = e.locked;
                ev.group = e.group;
            });
        });
        on_motion = b.pointer_motion().connect([this](lm::PointerMotionEvent& e) {
            push(kLiMotion, [&](LibinputEventRaw& ev) { ev.x = e.dx; ev.y = e.dy; });
        });
        on_button = b.pointer_button().connect([this](lm::PointerButtonEvent& e) {
            push(kLiButton, [&](LibinputEventRaw& ev) { ev.code = e.button; ev.pressed = e.pressed; });
        });
        on_axis = b.pointer_axis().connect([this](lm::PointerAxisEvent& e) {
            push(kLiAxis, [&](LibinputEventRaw& ev) {
                ev.x = e.dx;
                ev.y = e.dy;
                ev.steps_x = e.dx_steps;
                ev.steps_y = e.dy_steps;
                ev.flags = (e.stop_horizontal ? 1u : 0u) | (e.stop_vertical ? 2u : 0u);
            });
        });
        on_touch_down = b.touch_down().connect([this](lm::TouchDownEvent& e) {
            push(kLiTouchDown, [&](LibinputEventRaw& ev) { ev.id = e.id; ev.x = e.x; ev.y = e.y; });
        });
        on_touch_motion = b.touch_motion().connect([this](lm::TouchMotionEvent& e) {
            push(kLiTouchMotion, [&](LibinputEventRaw& ev) { ev.id = e.id; ev.x = e.x; ev.y = e.y; });
        });
        on_touch_up = b.touch_up().connect([this](lm::TouchUpEvent& e) {
            push(kLiTouchUp, [&](LibinputEventRaw& ev) { ev.id = e.id; });
        });
        on_touch_frame = b.touch_frame().connect([this](lm::TouchFrameEvent&) {
            push(kLiTouchFrame, [](LibinputEventRaw&) {});
        });
        on_touch_cancel = b.touch_cancel().connect([this](lm::TouchCancelEvent& e) {
            push(kLiTouchCancel, [&](LibinputEventRaw& ev) { ev.id = e.id; });
        });
        on_caps = b.capabilities_changed().connect([this](lm::InputCapabilities& c) {
            push(kLiCapabilities, [&](LibinputEventRaw& ev) {
                ev.flags = (c.keyboard ? 1u : 0u) | (c.pointer ? 2u : 0u) | (c.touch ? 4u : 0u);
            });
        });
    }
};

LibinputBackendShim::~LibinputBackendShim() = default;
LibinputBackendShim::LibinputBackendShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LibinputBackendShim::Impl& LibinputBackendShim::impl() const { return *impl_; }

namespace {

std::unique_ptr<LibinputBackendShim> make_libinput(const DisplayShim& display, lm::Session* session,
                                                   bool path, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto loop = display.impl().display->event_loop();
        auto r = path ? lm::LibinputBackend::create_path(loop, session)
                      : lm::LibinputBackend::create(loop, session);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<LibinputBackendShim::Impl>();
        impl->backend = std::make_unique<lm::LibinputBackend>(std::move(*r));
        impl->watch();
        return std::make_unique<LibinputBackendShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

} // namespace

std::unique_ptr<LibinputBackendShim> libinput_backend_new(const DisplayShim& display, const SessionShim& session, rust::String& error) {
    return make_libinput(display, session.impl().session.get(), false, error);
}

std::unique_ptr<LibinputBackendShim> libinput_backend_new_standalone(const DisplayShim& display, rust::String& error) {
    return make_libinput(display, nullptr, false, error);
}

std::unique_ptr<LibinputBackendShim> libinput_backend_new_path(const DisplayShim& display, const SessionShim& session, rust::String& error) {
    return make_libinput(display, session.impl().session.get(), true, error);
}

std::unique_ptr<LibinputBackendShim> libinput_backend_new_path_standalone(const DisplayShim& display, rust::String& error) {
    return make_libinput(display, nullptr, true, error);
}

bool LibinputBackendShim::start(rust::String& error) {
    if (!impl_->backend) { error = "backend not initialized"; return false; }
    auto st = impl_->backend->start();
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

bool LibinputBackendShim::add_device(rust::Str path, rust::String& error) {
    if (!impl_->backend) { error = "backend not initialized"; return false; }
    auto st = impl_->backend->add_device(std::string(path));
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

bool LibinputBackendShim::next_input_event(LibinputEventRaw& out) {
    if (impl_->events.empty()) return false;
    out = impl_->events.front();
    impl_->events.pop_front();
    return true;
}

rust::String LibinputBackendShim::keymap() const {
    return impl_->backend ? to_rust(impl_->backend->keymap()) : rust::String{};
}

bool LibinputBackendShim::set_keymap(rust::Str xkb_text) {
    return impl_->backend && impl_->backend->set_keymap(std::string(xkb_text));
}

std::uint32_t LibinputBackendShim::capabilities() const {
    if (!impl_->backend) return 0;
    const lm::InputCapabilities c = impl_->backend->capabilities();
    return (c.keyboard ? 1u : 0u) | (c.pointer ? 2u : 0u) | (c.touch ? 4u : 0u);
}

// ============================================================================
// Protocols
// ============================================================================

// 0 is Rust's "no surface". A live id never packs to 0 (generations start at
// 1), but SurfaceId{0, 0} would still pass valid(), which checks the index.
inline lm::SurfaceId id_from_u64(std::uint64_t raw) {
    if (raw == 0) return lm::SurfaceId{};
    return lm::SurfaceId{static_cast<std::uint32_t>(raw & 0xffffffff), static_cast<std::uint32_t>(raw >> 32)};
}

inline std::uint64_t u64_from_id(lm::SurfaceId id) {
    return id.valid() ? ((static_cast<std::uint64_t>(id.generation) << 32) | id.index) : 0;
}

std::int32_t surface_width(std::uint64_t surface_id) {
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    return s ? s->surface_width() : 0;
}
std::int32_t surface_height(std::uint64_t surface_id) {
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    return s ? s->surface_height() : 0;
}
std::int32_t buffer_width(std::uint64_t surface_id) {
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    return s ? s->buffer_width() : 0;
}
std::int32_t buffer_height(std::uint64_t surface_id) {
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    return s ? s->buffer_height() : 0;
}
std::int32_t buffer_scale(std::uint64_t surface_id) {
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    return s ? s->buffer_scale() : 1;
}
std::uint32_t buffer_transform(std::uint64_t surface_id) {
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    return s ? static_cast<std::uint32_t>(s->buffer_transform()) : 0;
}
bool surface_has_buffer(std::uint64_t surface_id) {
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    return s ? s->has_buffer() : false;
}
rust::Vec<Rect> surface_damage(std::uint64_t surface_id) {
    rust::Vec<Rect> out;
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    if (s) {
        for (const auto& b : s->damage()) {
            out.push_back(Rect{b.x, b.y, b.width, b.height});
        }
    }
    return out;
}
void surface_send_frame_done(std::uint64_t surface_id, std::uint32_t time_ms) {
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    if (s) s->send_frame_done(time_ms);
}
bool surface_accepts_input(std::uint64_t surface_id, double sx, double sy) {
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    return s ? s->accepts_input(sx, sy) : false;
}

struct CompositorShim::Impl {
    std::optional<lm::Compositor> comp;
};
CompositorShim::~CompositorShim() = default;
CompositorShim::CompositorShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CompositorShim::Impl& CompositorShim::impl() const { return *impl_; }

std::unique_ptr<CompositorShim> compositor_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::Compositor::create(*display.impl().display);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<CompositorShim::Impl>();
        impl->comp.emplace(std::move(*r));
        return std::make_unique<CompositorShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

struct SubcompositorShim::Impl {
    std::optional<lm::Subcompositor> subcomp;
};
SubcompositorShim::~SubcompositorShim() = default;
SubcompositorShim::SubcompositorShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SubcompositorShim::Impl& SubcompositorShim::impl() const { return *impl_; }

std::unique_ptr<SubcompositorShim> subcompositor_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::Subcompositor::create(*display.impl().display);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<SubcompositorShim::Impl>();
        impl->subcomp.emplace(std::move(*r));
        return std::make_unique<SubcompositorShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

namespace {

// An event for Rust to drain; `kind` says which of the other fields mean
// something. Every shim that forwards signals keeps one queue of these.
EventRaw event(EventKind kind, std::uint64_t id, std::uint64_t other = 0) {
    EventRaw e{};
    e.kind = kind;
    e.id = id;
    e.other = other;
    return e;
}

struct Events {
    std::deque<EventRaw> queue;

    void push(EventRaw e) { queue.push_back(std::move(e)); }
    bool pop(EventRaw& out) {
        if (queue.empty()) return false;
        out = std::move(queue.front());
        queue.pop_front();
        return true;
    }
};

// Identity for an object that has no surface of its own to be named by (a
// lock, an idle notification, a text input). Never reused.
std::uint64_t next_object_id() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

lm::Box to_box(const Rect& r) { return lm::Box{r.x, r.y, r.width, r.height}; }
Rect to_rect(const lm::Box& b) { return Rect{b.x, b.y, b.width, b.height}; }

std::uint64_t surface_u64(lm::Surface* s) { return s != nullptr ? u64_from_id(s->id()) : 0; }

rust::Vec<rust::String> to_rust(const std::vector<std::string>& v) {
    rust::Vec<rust::String> out;
    for (const auto& s : v) out.push_back(to_rust(s));
    return out;
}

lm::SceneItem scene_item(const SceneItemRaw& raw) {
    lm::SceneItem it{};
    it.kind = static_cast<lm::SceneItem::Kind>(raw.kind);
    it.box = lm::Box{raw.x, raw.y, raw.width, raw.height};
    it.surface = id_from_u64(raw.surface_id);
    it.x = raw.surface_x;
    it.y = raw.surface_y;
    it.color = lm::Color{raw.r, raw.g, raw.b, raw.a};
    it.thickness = raw.thickness;
    it.serial = raw.serial;
    it.tag = raw.tag;
    it.accepts_input = raw.accepts_input;
    it.alpha = raw.alpha;
    it.corner_radius = raw.corner_radius;
    it.blur.enabled = raw.blur_enabled;
    it.blur.xray = raw.blur_xray;
    it.shadow.color = lm::Color{raw.shadow_r, raw.shadow_g, raw.shadow_b, raw.shadow_a};
    it.shadow.feather = raw.shadow_feather;
    return it;
}

std::vector<lm::SceneItem> scene_items(rust::Slice<const SceneItemRaw> items) {
    std::vector<lm::SceneItem> scene;
    scene.reserve(items.size());
    for (const auto& raw : items) scene.push_back(scene_item(raw));
    return scene;
}

SceneItemRaw scene_item_raw(const lm::SceneItem& it) {
    SceneItemRaw raw{};
    raw.kind = static_cast<std::uint32_t>(it.kind);
    raw.x = it.box.x;
    raw.y = it.box.y;
    raw.width = it.box.width;
    raw.height = it.box.height;
    raw.surface_id = u64_from_id(it.surface);
    raw.surface_x = it.x;
    raw.surface_y = it.y;
    raw.r = it.color.r;
    raw.g = it.color.g;
    raw.b = it.color.b;
    raw.a = it.color.a;
    raw.thickness = it.thickness;
    raw.serial = it.serial;
    raw.tag = it.tag;
    raw.accepts_input = it.accepts_input;
    raw.alpha = it.alpha;
    raw.corner_radius = it.corner_radius;
    raw.blur_enabled = it.blur.enabled;
    raw.blur_xray = it.blur.xray;
    raw.shadow_r = it.shadow.color.r;
    raw.shadow_g = it.shadow.color.g;
    raw.shadow_b = it.shadow.color.b;
    raw.shadow_a = it.shadow.color.a;
    raw.shadow_feather = it.shadow.feather;
    return raw;
}

} // namespace

// Every toplevel and popup is watched from the moment the shell announces it
// until its destroy signal, which drops the watch; Rust names them by surface.
struct XdgShellShim::Impl {
    struct ToplevelEntry {
        lm::Toplevel* toplevel = nullptr;
        Conn<lm::ToplevelMap> on_map;
        Conn<lm::ToplevelUnmap> on_unmap;
        Conn<lm::ToplevelDestroy> on_destroy;
        Conn<lm::ToplevelRequestMaximize> on_maximize;
        Conn<lm::ToplevelRequestFullscreen> on_fullscreen;
        Conn<lm::ToplevelRequestMinimize> on_minimize;
        Conn<lm::ToplevelRequestMove> on_move;
        Conn<lm::ToplevelRequestResize> on_resize;
        Conn<lm::ToplevelRequestWindowMenu> on_window_menu;
        Conn<lm::ToplevelIdentityChange> on_identity;
        Conn<lm::ToplevelStateChange> on_state;
        Conn<lm::ToplevelParentChange> on_parent;
    };
    struct PopupEntry {
        lm::Popup* popup = nullptr;
        Conn<lm::PopupMap> on_map;
        Conn<lm::PopupUnmap> on_unmap;
        Conn<lm::PopupDestroy> on_destroy;
        Conn<lm::PopupReposition> on_reposition;
        Conn<lm::PopupGrab> on_grab;
    };
    // What set_popup_constraint_areas() was last told, for the query below:
    // the shell asks synchronously, and C++ never calls Rust.
    struct Area {
        std::uint64_t surface;
        lm::Box parent, usable;
    };

    std::optional<lm::XdgShell> shell;
    std::unordered_map<std::uint64_t, std::unique_ptr<ToplevelEntry>> toplevels;
    std::unordered_map<std::uint64_t, std::unique_ptr<PopupEntry>> popups;
    std::vector<Area> areas;
    Events events;
    Conn<lm::NewToplevel> on_new_toplevel;
    Conn<lm::NewPopup> on_new_popup;

    lm::Toplevel* toplevel(std::uint64_t id) const {
        const auto it = toplevels.find(id);
        return it == toplevels.end() ? nullptr : it->second->toplevel;
    }
    lm::Popup* popup(std::uint64_t id) const {
        const auto it = popups.find(id);
        return it == popups.end() ? nullptr : it->second->popup;
    }

    void watch(lm::Toplevel& t) {
        const std::uint64_t id = u64_from_id(t.surface().id());
        auto entry = std::make_unique<ToplevelEntry>();
        entry->toplevel = &t;
        auto plain = [this, id](EventKind kind) {
            return [this, id, kind](auto&) { events.push(event(kind, id)); };
        };
        entry->on_map = t.map.connect(plain(EventKind::ToplevelMap));
        entry->on_unmap = t.unmap.connect(plain(EventKind::ToplevelUnmap));
        entry->on_minimize = t.request_minimize.connect(plain(EventKind::ToplevelRequestMinimize));
        entry->on_identity = t.identity_change.connect(plain(EventKind::ToplevelIdentityChange));
        entry->on_state = t.state_change.connect(plain(EventKind::ToplevelStateChange));
        entry->on_maximize = t.request_maximize.connect([this, id](lm::ToplevelRequestMaximize& e) {
            EventRaw ev = event(EventKind::ToplevelRequestMaximize, id);
            ev.flag = e.maximized;
            events.push(std::move(ev));
        });
        entry->on_fullscreen = t.request_fullscreen.connect([this, id](lm::ToplevelRequestFullscreen& e) {
            EventRaw ev = event(EventKind::ToplevelRequestFullscreen, id);
            ev.flag = e.fullscreen;
            events.push(std::move(ev));
        });
        entry->on_move = t.request_move.connect([this, id](lm::ToplevelRequestMove& e) {
            EventRaw ev = event(EventKind::ToplevelRequestMove, id);
            ev.serial = e.serial;
            events.push(std::move(ev));
        });
        entry->on_resize = t.request_resize.connect([this, id](lm::ToplevelRequestResize& e) {
            EventRaw ev = event(EventKind::ToplevelRequestResize, id);
            ev.serial = e.serial;
            ev.value = e.edges;
            events.push(std::move(ev));
        });
        entry->on_window_menu = t.request_window_menu.connect([this, id](lm::ToplevelRequestWindowMenu& e) {
            EventRaw ev = event(EventKind::ToplevelRequestWindowMenu, id);
            ev.serial = e.serial;
            ev.x = e.x;
            ev.y = e.y;
            events.push(std::move(ev));
        });
        entry->on_parent = t.parent_change.connect([this, id](lm::ToplevelParentChange& e) {
            events.push(event(EventKind::ToplevelParentChange, id,
                              e.parent != nullptr ? surface_u64(&e.parent->surface()) : 0));
        });
        // Erasing the entry inside its own slot is safe: a Signal never frees
        // the slot that is running.
        entry->on_destroy = t.destroy.connect([this, id](lm::ToplevelDestroy&) {
            events.push(event(EventKind::ToplevelDestroy, id));
            toplevels.erase(id);
        });
        events.push(event(EventKind::NewToplevel, id));
        toplevels[id] = std::move(entry);
    }

    void watch(lm::Popup& p) {
        const std::uint64_t id = u64_from_id(p.surface().id());
        auto entry = std::make_unique<PopupEntry>();
        entry->popup = &p;
        auto plain = [this, id](EventKind kind) {
            return [this, id, kind](auto&) { events.push(event(kind, id)); };
        };
        entry->on_map = p.map.connect(plain(EventKind::PopupMap));
        entry->on_unmap = p.unmap.connect(plain(EventKind::PopupUnmap));
        entry->on_reposition = p.reposition.connect(plain(EventKind::PopupReposition));
        entry->on_grab = p.grab.connect([this, id](lm::PopupGrab& e) {
            EventRaw ev = event(EventKind::PopupGrab, id);
            ev.serial = e.serial;
            events.push(std::move(ev));
        });
        entry->on_destroy = p.destroy.connect([this, id](lm::PopupDestroy&) {
            events.push(event(EventKind::PopupDestroy, id));
            popups.erase(id);
        });
        events.push(event(EventKind::NewPopup, id, surface_u64(p.parent_surface())));
        popups[id] = std::move(entry);
    }
};
XdgShellShim::~XdgShellShim() = default;
XdgShellShim::XdgShellShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
XdgShellShim::Impl& XdgShellShim::impl() const { return *impl_; }

std::unique_ptr<XdgShellShim> xdg_shell_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::XdgShell::create(*display.impl().display);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<XdgShellShim::Impl>();
        impl->shell.emplace(std::move(*r));
        auto* raw = impl.get();
        raw->on_new_toplevel = raw->shell->new_toplevel().connect(
            [raw](lm::NewToplevel& e) { raw->watch(e.toplevel); });
        raw->on_new_popup = raw->shell->new_popup().connect(
            [raw](lm::NewPopup& e) { raw->watch(e.popup); });
        raw->shell->set_popup_constraint_query(
            [raw](lm::Surface& parent, lm::Box& parent_box, lm::Box& usable) {
                const std::uint64_t id = u64_from_id(parent.id());
                for (const auto& a : raw->areas) {
                    if (a.surface == id) {
                        parent_box = a.parent;
                        usable = a.usable;
                        return !usable.empty();
                    }
                }
                return false;
            });
        return std::make_unique<XdgShellShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

void XdgShellShim::set_bounds(std::int32_t width, std::int32_t height) const {
    if (impl_->shell) impl_->shell->set_bounds(width, height);
}

bool XdgShellShim::next_event(EventRaw& out) const { return impl_->events.pop(out); }

rust::Vec<std::uint64_t> XdgShellShim::toplevels() const {
    rust::Vec<std::uint64_t> out;
    for (const auto& [id, entry] : impl_->toplevels) out.push_back(id);
    return out;
}

// Bits of ToplevelStates, as src/protocol/xdg_shell.rs spells them.
enum : std::uint32_t {
    kMaximized = 1, kFullscreen = 2, kActivated = 4, kResizing = 8, kTiled = 16, kMinimized = 32,
};

bool XdgShellShim::toplevel_info(std::uint64_t surface_id, ToplevelInfoRaw& out) const {
    const lm::Toplevel* t = impl_->toplevel(surface_id);
    if (t == nullptr) return false;
    out.parent = t->parent() != nullptr ? surface_u64(&t->parent()->surface()) : 0;
    out.mapped = t->mapped();
    out.min_width = t->min_width();
    out.min_height = t->min_height();
    out.max_width = t->max_width();
    out.max_height = t->max_height();
    const lm::XdgGeometry g = t->geometry();
    out.geometry = Rect{g.x, g.y, g.width, g.height};
    out.states = (t->is_maximized() ? kMaximized : 0u) | (t->is_fullscreen() ? kFullscreen : 0u) |
                 (t->is_activated() ? kActivated : 0u) | (t->is_resizing() ? kResizing : 0u) |
                 (t->is_tiled() ? kTiled : 0u) | (t->is_minimized() ? kMinimized : 0u);
    out.pending_width = t->pending_width();
    out.pending_height = t->pending_height();
    return true;
}

rust::String XdgShellShim::toplevel_title(std::uint64_t surface_id) const {
    const lm::Toplevel* t = impl_->toplevel(surface_id);
    return t != nullptr ? to_rust(t->title()) : rust::String{};
}

rust::String XdgShellShim::toplevel_app_id(std::uint64_t surface_id) const {
    const lm::Toplevel* t = impl_->toplevel(surface_id);
    return t != nullptr ? to_rust(t->app_id()) : rust::String{};
}

std::uint32_t XdgShellShim::toplevel_configure(std::uint64_t surface_id, std::int32_t width, std::int32_t height) const {
    lm::Toplevel* t = impl_->toplevel(surface_id);
    return t != nullptr ? t->configure(width, height) : 0;
}

void XdgShellShim::toplevel_set_state(std::uint64_t surface_id, std::uint32_t state, bool on) const {
    lm::Toplevel* t = impl_->toplevel(surface_id);
    if (t == nullptr) return;
    switch (state) {
    case kMaximized: t->set_maximized(on); break;
    case kFullscreen: t->set_fullscreen(on); break;
    case kActivated: t->set_activated(on); break;
    case kResizing: t->set_resizing(on); break;
    case kTiled: t->set_tiled(on); break;
    case kMinimized: t->set_minimized(on); break;
    default: break;
    }
}

void XdgShellShim::toplevel_close(std::uint64_t surface_id) const {
    if (lm::Toplevel* t = impl_->toplevel(surface_id); t != nullptr) t->close();
}

bool XdgShellShim::popup_info(std::uint64_t surface_id, PopupInfoRaw& out) const {
    const lm::Popup* p = impl_->popup(surface_id);
    if (p == nullptr) return false;
    out.parent = surface_u64(p->parent_surface());
    out.mapped = p->mapped();
    out.has_grab = p->has_grab();
    out.x = p->x();
    out.y = p->y();
    out.width = p->width();
    out.height = p->height();
    return true;
}

void XdgShellShim::popup_dismiss(std::uint64_t surface_id) const {
    if (lm::Popup* p = impl_->popup(surface_id); p != nullptr) p->dismiss();
}

void XdgShellShim::set_popup_constraint_areas(rust::Slice<const PopupAreaRaw> areas) const {
    impl_->areas.clear();
    for (const auto& a : areas) {
        impl_->areas.push_back(Impl::Area{a.surface_id, to_box(a.parent), to_box(a.usable)});
    }
}

struct SeatShim::Impl {
    std::optional<lm::Seat> seat;
    Events events;
    Conn<lm::SeatKeyboardFocus> on_keyboard_focus;
    Conn<lm::SeatPointerFocus> on_pointer_focus;
    Conn<lm::SeatCursorChange> on_cursor;
};
SeatShim::~SeatShim() = default;
SeatShim::SeatShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SeatShim::Impl& SeatShim::impl() const { return *impl_; }

std::unique_ptr<SeatShim> seat_new(const DisplayShim& display, rust::Str name, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::Seat::create(*display.impl().display, std::string(name));
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<SeatShim::Impl>();
        impl->seat.emplace(std::move(*r));
        auto* raw = impl.get();
        raw->on_keyboard_focus = raw->seat->keyboard_focus_changed().connect([raw](lm::SeatKeyboardFocus& e) {
            raw->events.push(event(EventKind::SeatKeyboardFocus, u64_from_id(e.surface)));
        });
        raw->on_pointer_focus = raw->seat->pointer_focus_changed().connect([raw](lm::SeatPointerFocus& e) {
            raw->events.push(event(EventKind::SeatPointerFocus, u64_from_id(e.surface)));
        });
        raw->on_cursor = raw->seat->cursor_changed().connect([raw](lm::SeatCursorChange& e) {
            EventRaw ev = event(EventKind::SeatCursorChange, u64_from_id(e.surface));
            ev.x = e.hotspot_x;
            ev.y = e.hotspot_y;
            ev.flag = e.hidden;
            raw->events.push(std::move(ev));
        });
        return std::make_unique<SeatShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

void SeatShim::set_capabilities(bool keyboard, bool pointer, bool touch) const {
    if (impl_->seat) impl_->seat->set_capabilities(keyboard, pointer, touch);
}
bool SeatShim::set_keymap(rust::Str xkb_text) const {
    if (!impl_->seat) return false;
    return impl_->seat->set_keymap(std::string(xkb_text));
}
rust::String SeatShim::keymap() const {
    if (!impl_->seat) return "";
    return to_rust(impl_->seat->keymap());
}
void SeatShim::set_keyboard_focus(std::uint64_t surface_id) const {
    if (!impl_->seat) return;
    lm::Surface* s = surface_id != 0 ? lm::surface_from_id(id_from_u64(surface_id)) : nullptr;
    impl_->seat->set_keyboard_focus(s);
}
std::uint64_t SeatShim::keyboard_focus() const {
    if (!impl_->seat) return 0;
    return u64_from_id(impl_->seat->keyboard_focus());
}
void SeatShim::notify_key(std::uint32_t key, bool pressed) const {
    if (impl_->seat) impl_->seat->notify_key(key, pressed);
}
void SeatShim::notify_modifiers(std::uint32_t depressed, std::uint32_t latched,
                               std::uint32_t locked, std::uint32_t group) const {
    if (impl_->seat) impl_->seat->notify_modifiers(depressed, latched, locked, group);
}
void SeatShim::pointer_enter(std::uint64_t surface_id, double sx, double sy) const {
    if (!impl_->seat) return;
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    if (s) impl_->seat->pointer_enter(*s, sx, sy);
}
void SeatShim::pointer_clear_focus() const {
    if (impl_->seat) impl_->seat->pointer_clear_focus();
}
std::uint64_t SeatShim::pointer_focus() const {
    if (!impl_->seat) return 0;
    return u64_from_id(impl_->seat->pointer_focus());
}
std::uint64_t SeatShim::pointer_grab() const {
    if (!impl_->seat) return 0;
    return u64_from_id(impl_->seat->pointer_grab());
}
void SeatShim::pointer_motion(double sx, double sy) const {
    if (impl_->seat) impl_->seat->pointer_motion(sx, sy);
}
void SeatShim::pointer_button(std::uint32_t button, bool pressed) const {
    if (impl_->seat) impl_->seat->pointer_button(button, pressed);
}
void SeatShim::pointer_axis(double dx, double dy) const {
    if (impl_->seat) impl_->seat->pointer_axis(dx, dy);
}
void SeatShim::pointer_axis_discrete(std::int32_t dx_steps, std::int32_t dy_steps) const {
    if (impl_->seat) impl_->seat->pointer_axis_discrete(dx_steps, dy_steps);
}
void SeatShim::pointer_axis_stop(bool horizontal, bool vertical) const {
    if (impl_->seat) impl_->seat->pointer_axis_stop(horizontal, vertical);
}
std::uint64_t SeatShim::cursor_surface() const {
    if (!impl_->seat) return 0;
    return u64_from_id(impl_->seat->cursor_surface());
}
bool SeatShim::cursor_hidden() const {
    return impl_->seat ? impl_->seat->cursor_hidden() : false;
}
std::int32_t SeatShim::cursor_hotspot_x() const {
    return impl_->seat ? impl_->seat->cursor_hotspot_x() : 0;
}
std::int32_t SeatShim::cursor_hotspot_y() const {
    return impl_->seat ? impl_->seat->cursor_hotspot_y() : 0;
}
void SeatShim::touch_down(std::uint64_t surface_id, std::int32_t id, double x, double y) const {
    if (!impl_->seat) return;
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    if (s) impl_->seat->touch_down(*s, id, x, y);
}
void SeatShim::touch_motion(std::int32_t id, double x, double y) const {
    if (impl_->seat) impl_->seat->touch_motion(id, x, y);
}
void SeatShim::touch_up(std::int32_t id) const {
    if (impl_->seat) impl_->seat->touch_up(id);
}
void SeatShim::touch_frame() const {
    if (impl_->seat) impl_->seat->touch_frame();
}
void SeatShim::touch_cancel() const {
    if (impl_->seat) impl_->seat->touch_cancel();
}
std::uint64_t SeatShim::touch_focus(std::int32_t id) const {
    if (!impl_->seat) return 0;
    return u64_from_id(impl_->seat->touch_focus(id));
}
bool SeatShim::next_event(EventRaw& out) const { return impl_->events.pop(out); }

struct LayerShellShim::Impl {
    struct Entry {
        lm::LayerSurface* surface = nullptr;
        Conn<lm::LayerSurfaceMap> on_map;
        Conn<lm::LayerSurfaceUnmap> on_unmap;
        Conn<lm::LayerSurfaceDestroy> on_destroy;
        Conn<lm::LayerSurfaceStateChange> on_state;
        Conn<lm::LayerSurfacePopup> on_popup;
    };

    std::optional<lm::LayerShell> layer_shell;
    std::unordered_map<std::uint64_t, std::unique_ptr<Entry>> surfaces;
    Events events;
    Conn<lm::NewLayerSurface> on_new;

    lm::LayerSurface* find(std::uint64_t id) const {
        const auto it = surfaces.find(id);
        return it == surfaces.end() ? nullptr : it->second->surface;
    }

    void watch(lm::LayerSurface& ls) {
        const std::uint64_t id = u64_from_id(ls.surface().id());
        auto entry = std::make_unique<Entry>();
        entry->surface = &ls;
        auto plain = [this, id](EventKind kind) {
            return [this, id, kind](auto&) { events.push(event(kind, id)); };
        };
        entry->on_map = ls.map.connect(plain(EventKind::LayerSurfaceMap));
        entry->on_unmap = ls.unmap.connect(plain(EventKind::LayerSurfaceUnmap));
        entry->on_state = ls.state_change.connect(plain(EventKind::LayerSurfaceStateChange));
        entry->on_popup = ls.new_popup.connect([this, id](lm::LayerSurfacePopup& e) {
            events.push(event(EventKind::LayerSurfacePopup, id, u64_from_id(e.popup.surface().id())));
        });
        entry->on_destroy = ls.destroy.connect([this, id](lm::LayerSurfaceDestroy&) {
            events.push(event(EventKind::LayerSurfaceDestroy, id));
            surfaces.erase(id);
        });
        events.push(event(EventKind::NewLayerSurface, id));
        surfaces[id] = std::move(entry);
    }
};
LayerShellShim::~LayerShellShim() = default;
LayerShellShim::LayerShellShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LayerShellShim::Impl& LayerShellShim::impl() const { return *impl_; }

std::unique_ptr<LayerShellShim> layer_shell_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::LayerShell::create(*display.impl().display);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<LayerShellShim::Impl>();
        impl->layer_shell.emplace(std::move(*r));
        auto* raw = impl.get();
        raw->on_new = raw->layer_shell->new_layer_surface().connect(
            [raw](lm::NewLayerSurface& e) { raw->watch(e.layer_surface); });
        return std::make_unique<LayerShellShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

bool LayerShellShim::next_event(EventRaw& out) const { return impl_->events.pop(out); }

rust::Vec<std::uint64_t> LayerShellShim::layer_surfaces() const {
    rust::Vec<std::uint64_t> out;
    for (const auto& [id, entry] : impl_->surfaces) out.push_back(id);
    return out;
}

bool LayerShellShim::info(std::uint64_t surface_id, LayerSurfaceInfoRaw& out) const {
    const lm::LayerSurface* ls = impl_->find(surface_id);
    if (ls == nullptr) return false;
    out.layer = static_cast<std::uint32_t>(ls->layer());
    out.anchor = ls->anchor();
    out.desired_width = ls->desired_width();
    out.desired_height = ls->desired_height();
    out.exclusive_zone = ls->exclusive_zone();
    out.exclusive_edge = ls->exclusive_edge();
    out.margin_top = ls->margin_top();
    out.margin_right = ls->margin_right();
    out.margin_bottom = ls->margin_bottom();
    out.margin_left = ls->margin_left();
    out.keyboard_interactivity = static_cast<std::uint32_t>(ls->keyboard_interactivity());
    out.mapped = ls->mapped();
    out.pending_width = ls->pending_width();
    out.pending_height = ls->pending_height();
    out.has_output = ls->output_resource() != nullptr;
    return true;
}

rust::String LayerShellShim::scope(std::uint64_t surface_id) const {
    const lm::LayerSurface* ls = impl_->find(surface_id);
    return ls != nullptr ? to_rust(ls->scope()) : rust::String{};
}

std::uint32_t LayerShellShim::configure(std::uint64_t surface_id, std::int32_t width, std::int32_t height) const {
    lm::LayerSurface* ls = impl_->find(surface_id);
    return ls != nullptr ? ls->configure(width, height) : 0;
}

void LayerShellShim::close(std::uint64_t surface_id) const {
    if (lm::LayerSurface* ls = impl_->find(surface_id); ls != nullptr) ls->close();
}

Rect LayerShellShim::arrange(std::uint64_t surface_id, Rect full, Rect& usable) const {
    lm::LayerSurface* ls = impl_->find(surface_id);
    if (ls == nullptr) return Rect{0, 0, 0, 0};
    lm::Box u = to_box(usable);
    const lm::Box placed = lm::arrange_layer_surface(*ls, to_box(full), u);
    usable = to_rect(u);
    return to_rect(placed);
}

struct OutputGlobalShim::Impl {
    std::optional<lm::OutputGlobal> global;
};
OutputGlobalShim::~OutputGlobalShim() = default;
OutputGlobalShim::OutputGlobalShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
OutputGlobalShim::Impl& OutputGlobalShim::impl() const { return *impl_; }

std::unique_ptr<OutputGlobalShim> output_global_new(const DisplayShim& display, std::int32_t width,
                                                   std::int32_t height, rust::Str name, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::OutputGlobal::create(*display.impl().display, width, height, std::string(name));
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<OutputGlobalShim::Impl>();
        impl->global.emplace(std::move(*r));
        return std::make_unique<OutputGlobalShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

void OutputGlobalShim::set_logical_position(std::int32_t x, std::int32_t y) const {
    if (impl_->global) impl_->global->set_logical_position(x, y);
}
void OutputGlobalShim::set_mode(std::int32_t width, std::int32_t height, std::int32_t refresh_mhz) const {
    if (impl_->global) impl_->global->set_mode(width, height, refresh_mhz);
}
void OutputGlobalShim::set_scale(std::int32_t scale) const {
    if (impl_->global) impl_->global->set_scale(scale);
}
void OutputGlobalShim::set_transform(std::uint32_t transform) const {
    if (impl_->global) impl_->global->set_transform(static_cast<lm::Transform>(transform));
}

namespace {

// Whether `resource` (a client's wl_output) is `global`'s.
bool is_output(wl_resource* resource, const OutputGlobalShim& global) {
    const auto& g = global.impl().global;
    return resource != nullptr && g && g->resource_for(wl_resource_get_client(resource)) == resource;
}

} // namespace

bool LayerShellShim::on_output(std::uint64_t surface_id, const OutputGlobalShim& output) const {
    const lm::LayerSurface* ls = impl_->find(surface_id);
    return ls != nullptr && is_output(ls->output_resource(), output);
}

struct CursorThemeShim::Impl {
    std::optional<lm::CursorTheme> theme;
};
CursorThemeShim::~CursorThemeShim() = default;
CursorThemeShim::CursorThemeShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CursorThemeShim::Impl& CursorThemeShim::impl() const { return *impl_; }

std::unique_ptr<CursorThemeShim> cursor_theme_load(rust::Str name, std::int32_t size, rust::String& error) {
    try {
        auto r = lm::CursorTheme::load(std::string(name), size);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<CursorThemeShim::Impl>();
        impl->theme.emplace(std::move(*r));
        return std::make_unique<CursorThemeShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

#define DEFINE_GLOBAL_SHIM_IMPL(Name, CppType, FuncName) \
struct Name##Shim::Impl { std::optional<lm::CppType> mgr; }; \
Name##Shim::~Name##Shim() = default; \
Name##Shim::Name##Shim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {} \
Name##Shim::Impl& Name##Shim::impl() const { return *impl_; } \
std::unique_ptr<Name##Shim> FuncName(const DisplayShim& display, rust::String& error) { \
    try { \
        if (!display.impl().display) { error = "display not initialized"; return nullptr; } \
        auto r = lm::CppType::create(*display.impl().display); \
        if (!r) { error = to_rust(r.error().message); return nullptr; } \
        auto impl = std::make_unique<Name##Shim::Impl>(); \
        impl->mgr.emplace(std::move(*r)); \
        return std::make_unique<Name##Shim>(std::move(impl)); \
    } catch (const std::exception& e) { error = e.what(); return nullptr; } \
}

DEFINE_GLOBAL_SHIM_IMPL(Viewporter, Viewporter, viewporter_new)
DEFINE_GLOBAL_SHIM_IMPL(FractionalScaleManager, FractionalScaleManager, fractional_scale_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(XdgDecorationManager, XdgDecorationManager, xdg_decoration_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(CursorShapeManager, CursorShapeManager, cursor_shape_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(SinglePixelBufferManager, SinglePixelBufferManager, single_pixel_buffer_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(TearingControlManager, TearingControlManager, tearing_control_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(ContentTypeManager, ContentTypeManager, content_type_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(FifoManager, FifoManager, fifo_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(CommitTimingManager, CommitTimingManager, commit_timing_manager_new)
// Presentation is defined by hand below SceneRendererShim, which links to it.
DEFINE_GLOBAL_SHIM_IMPL(BackgroundEffectManager, BackgroundEffectManager, background_effect_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(XdgToplevelIconManager, XdgToplevelIconManager, xdg_toplevel_icon_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(DrmSyncobjManager, DrmSyncobjManager, drm_syncobj_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(ScreencopyManager, ScreencopyManager, screencopy_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(ForeignToplevelManager, ForeignToplevelManager, foreign_toplevel_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(WorkspaceManager, WorkspaceManager, workspace_manager_new)

#undef DEFINE_GLOBAL_SHIM_IMPL

// ---- xdg-activation --------------------------------------------------------

// Only request_activate is forwarded. A token request wants its answer inside
// the signal, which Rust cannot give; luminaria's default (grant) stands.
struct XdgActivationShim::Impl {
    std::optional<lm::XdgActivation> mgr;
    Events events;
    Conn<lm::ActivationRequest> on_activate;
};
XdgActivationShim::~XdgActivationShim() = default;
XdgActivationShim::XdgActivationShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
XdgActivationShim::Impl& XdgActivationShim::impl() const { return *impl_; }
std::unique_ptr<XdgActivationShim> xdg_activation_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::XdgActivation::create(*display.impl().display);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<XdgActivationShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        auto* raw = impl.get();
        raw->on_activate = raw->mgr->request_activate().connect([raw](lm::ActivationRequest& e) {
            EventRaw ev = event(EventKind::ActivationRequest, u64_from_id(e.surface.id()),
                                surface_u64(e.requesting_surface));
            ev.flag = e.token_valid;
            ev.serial = e.serial;
            ev.text = to_rust(e.app_id);
            raw->events.push(std::move(ev));
        });
        return std::make_unique<XdgActivationShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}
bool XdgActivationShim::next_event(EventRaw& out) const { return impl_->events.pop(out); }

// ---- idle-inhibit ----------------------------------------------------------

struct IdleInhibitManagerShim::Impl {
    struct Entry {
        lm::IdleInhibitor* inhibitor = nullptr;
        Conn<lm::IdleInhibitorDestroy> on_destroy;
    };
    std::optional<lm::IdleInhibitManager> mgr;
    std::unordered_map<std::uint64_t, Entry> inhibitors;
    Events events;
    Conn<lm::NewIdleInhibitor> on_new;
    Conn<lm::IdleInhibitChange> on_change;

    lm::IdleInhibitor* find(std::uint64_t id) const {
        const auto it = inhibitors.find(id);
        return it == inhibitors.end() ? nullptr : it->second.inhibitor;
    }
};
IdleInhibitManagerShim::~IdleInhibitManagerShim() = default;
IdleInhibitManagerShim::IdleInhibitManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
IdleInhibitManagerShim::Impl& IdleInhibitManagerShim::impl() const { return *impl_; }
std::unique_ptr<IdleInhibitManagerShim> idle_inhibit_manager_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::IdleInhibitManager::create(*display.impl().display);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<IdleInhibitManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        auto* raw = impl.get();
        raw->on_new = raw->mgr->new_inhibitor().connect([raw](lm::NewIdleInhibitor& e) {
            const std::uint64_t id = next_object_id();
            auto& entry = raw->inhibitors[id];
            entry.inhibitor = &e.inhibitor;
            entry.on_destroy = e.inhibitor.destroy.connect([raw, id](lm::IdleInhibitorDestroy&) {
                raw->events.push(event(EventKind::IdleInhibitorDestroy, id));
                raw->inhibitors.erase(id);
            });
            raw->events.push(event(EventKind::NewIdleInhibitor, id, u64_from_id(e.inhibitor.surface().id())));
        });
        raw->on_change = raw->mgr->changed().connect([raw](lm::IdleInhibitChange& e) {
            EventRaw ev = event(EventKind::IdleInhibitChange, 0);
            ev.flag = e.inhibited;
            raw->events.push(std::move(ev));
        });
        return std::make_unique<IdleInhibitManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}
bool IdleInhibitManagerShim::next_event(EventRaw& out) const { return impl_->events.pop(out); }
bool IdleInhibitManagerShim::inhibited() const { return impl_->mgr && impl_->mgr->inhibited(); }
std::size_t IdleInhibitManagerShim::inhibitor_count() const { return impl_->mgr ? impl_->mgr->inhibitor_count() : 0; }
std::uint64_t IdleInhibitManagerShim::inhibitor_surface(std::uint64_t inhibitor) const {
    lm::IdleInhibitor* i = impl_->find(inhibitor);
    return i != nullptr ? u64_from_id(i->surface().id()) : 0;
}
bool IdleInhibitManagerShim::inhibitor_visible(std::uint64_t inhibitor) const {
    const lm::IdleInhibitor* i = impl_->find(inhibitor);
    return i != nullptr && i->visible();
}
void IdleInhibitManagerShim::set_inhibitor_visible(std::uint64_t inhibitor, bool visible) const {
    if (lm::IdleInhibitor* i = impl_->find(inhibitor); i != nullptr) i->set_visible(visible);
}

// ---- ext-idle-notify -------------------------------------------------------

struct IdleNotifierShim::Impl {
    struct Entry {
        lm::IdleNotification* notification = nullptr;
        Conn<lm::IdleNotificationDestroy> on_destroy;
    };
    std::optional<lm::IdleNotifier> mgr;
    std::unordered_map<std::uint64_t, Entry> notifications;
    Events events;
    Conn<lm::NewIdleNotification> on_new;
    Conn<lm::IdleStateChange> on_state;

    std::uint64_t id_of(const lm::IdleNotification* n) const {
        for (const auto& [id, entry] : notifications) {
            if (entry.notification == n) return id;
        }
        return 0;
    }
};
IdleNotifierShim::~IdleNotifierShim() = default;
IdleNotifierShim::IdleNotifierShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
IdleNotifierShim::Impl& IdleNotifierShim::impl() const { return *impl_; }
std::unique_ptr<IdleNotifierShim> idle_notifier_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::IdleNotifier::create(*display.impl().display, display.impl().display->event_loop());
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<IdleNotifierShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        auto* raw = impl.get();
        raw->on_new = raw->mgr->new_notification().connect([raw](lm::NewIdleNotification& e) {
            const std::uint64_t id = next_object_id();
            auto& entry = raw->notifications[id];
            entry.notification = &e.notification;
            entry.on_destroy = e.notification.destroy.connect([raw, id](lm::IdleNotificationDestroy&) {
                raw->events.push(event(EventKind::IdleNotificationDestroy, id));
                raw->notifications.erase(id);
            });
            raw->events.push(event(EventKind::NewIdleNotification, id));
        });
        raw->on_state = raw->mgr->state_change().connect([raw](lm::IdleStateChange& e) {
            EventRaw ev = event(EventKind::IdleStateChange, raw->id_of(&e.notification));
            ev.flag = e.idled;
            raw->events.push(std::move(ev));
        });
        return std::make_unique<IdleNotifierShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}
bool IdleNotifierShim::next_event(EventRaw& out) const { return impl_->events.pop(out); }
void IdleNotifierShim::notify_activity() const { if (impl_->mgr) impl_->mgr->notify_activity(); }
void IdleNotifierShim::set_inhibited(bool inhibited) const { if (impl_->mgr) impl_->mgr->set_inhibited(inhibited); }
bool IdleNotifierShim::inhibited() const { return impl_->mgr && impl_->mgr->inhibited(); }
std::size_t IdleNotifierShim::notification_count() const { return impl_->mgr ? impl_->mgr->notification_count() : 0; }
std::size_t IdleNotifierShim::idled_count() const { return impl_->mgr ? impl_->mgr->idled_count() : 0; }
bool IdleNotifierShim::notification_info(std::uint64_t notification, IdleNotificationInfoRaw& out) const {
    const auto it = impl_->notifications.find(notification);
    if (it == impl_->notifications.end()) return false;
    const lm::IdleNotification& n = *it->second.notification;
    out.timeout_ms = n.timeout_ms();
    out.idled = n.idled();
    out.input_only = n.input_only();
    return true;
}

// ---- ext-session-lock ------------------------------------------------------

struct SessionLockManagerShim::Impl {
    struct LockEntry {
        lm::SessionLock* lock = nullptr;
        Conn<lm::NewSessionLockSurface> on_surface;
        Conn<lm::SessionLockDestroy> on_destroy;
    };
    struct SurfaceEntry {
        lm::SessionLockSurface* surface = nullptr;
        std::uint64_t lock = 0;
        Conn<lm::SessionLockSurfaceMap> on_map;
        Conn<lm::SessionLockSurfaceDestroy> on_destroy;
    };

    std::optional<lm::SessionLockManager> mgr;
    std::unordered_map<std::uint64_t, std::unique_ptr<LockEntry>> locks;
    std::unordered_map<std::uint64_t, std::unique_ptr<SurfaceEntry>> surfaces;
    Events events;
    Conn<lm::NewSessionLock> on_new_lock;

    lm::SessionLock* lock(std::uint64_t id) const {
        const auto it = locks.find(id);
        return it == locks.end() ? nullptr : it->second->lock;
    }
    const SurfaceEntry* surface(std::uint64_t id) const {
        const auto it = surfaces.find(id);
        return it == surfaces.end() ? nullptr : it->second.get();
    }

    void watch(lm::SessionLockSurface& ls, std::uint64_t lock_id) {
        const std::uint64_t id = u64_from_id(ls.surface().id());
        auto entry = std::make_unique<SurfaceEntry>();
        entry->surface = &ls;
        entry->lock = lock_id;
        entry->on_map = ls.map.connect([this, id](lm::SessionLockSurfaceMap&) {
            events.push(event(EventKind::LockSurfaceMap, id));
        });
        entry->on_destroy = ls.destroy.connect([this, id](lm::SessionLockSurfaceDestroy&) {
            events.push(event(EventKind::LockSurfaceDestroy, id));
            surfaces.erase(id);
        });
        events.push(event(EventKind::NewLockSurface, lock_id, id));
        surfaces[id] = std::move(entry);
    }

    void watch(lm::SessionLock& lock) {
        const std::uint64_t id = next_object_id();
        auto entry = std::make_unique<LockEntry>();
        entry->lock = &lock;
        entry->on_surface = lock.new_surface.connect(
            [this, id](lm::NewSessionLockSurface& e) { watch(e.lock_surface, id); });
        entry->on_destroy = lock.destroy.connect([this, id](lm::SessionLockDestroy& e) {
            EventRaw ev = event(EventKind::SessionLockDestroy, id);
            ev.flag = e.unlocked;
            events.push(std::move(ev));
            locks.erase(id);
        });
        events.push(event(EventKind::NewSessionLock, id));
        locks[id] = std::move(entry);
    }
};
SessionLockManagerShim::~SessionLockManagerShim() = default;
SessionLockManagerShim::SessionLockManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SessionLockManagerShim::Impl& SessionLockManagerShim::impl() const { return *impl_; }
std::unique_ptr<SessionLockManagerShim> session_lock_manager_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::SessionLockManager::create(*display.impl().display);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<SessionLockManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        auto* raw = impl.get();
        raw->on_new_lock = raw->mgr->new_lock().connect([raw](lm::NewSessionLock& e) { raw->watch(e.lock); });
        return std::make_unique<SessionLockManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}
bool SessionLockManagerShim::next_event(EventRaw& out) const { return impl_->events.pop(out); }
bool SessionLockManagerShim::locked() const { return impl_->mgr && impl_->mgr->session_locked(); }
std::uint64_t SessionLockManagerShim::current() const {
    if (!impl_->mgr) return 0;
    const lm::SessionLock* current = impl_->mgr->current();
    for (const auto& [id, entry] : impl_->locks) {
        if (current != nullptr && entry->lock == current) return id;
    }
    return 0;
}
bool SessionLockManagerShim::lock_locked(std::uint64_t lock) const {
    const lm::SessionLock* l = impl_->lock(lock);
    return l != nullptr && l->locked();
}
void SessionLockManagerShim::send_locked(std::uint64_t lock) const {
    if (lm::SessionLock* l = impl_->lock(lock); l != nullptr) l->send_locked();
}
void SessionLockManagerShim::send_finished(std::uint64_t lock) const {
    if (lm::SessionLock* l = impl_->lock(lock); l != nullptr) l->send_finished();
}
rust::Vec<std::uint64_t> SessionLockManagerShim::lock_surfaces(std::uint64_t lock) const {
    rust::Vec<std::uint64_t> out;
    if (const lm::SessionLock* l = impl_->lock(lock); l != nullptr) {
        for (lm::SessionLockSurface* s : l->surfaces()) out.push_back(u64_from_id(s->surface().id()));
    }
    return out;
}
bool SessionLockManagerShim::lock_surface_info(std::uint64_t surface_id, LockSurfaceInfoRaw& out) const {
    const Impl::SurfaceEntry* e = impl_->surface(surface_id);
    if (e == nullptr) return false;
    out.lock = e->lock;
    out.mapped = e->surface->mapped();
    out.width = e->surface->width();
    out.height = e->surface->height();
    out.has_output = e->surface->output_resource() != nullptr;
    return true;
}
std::uint32_t SessionLockManagerShim::lock_surface_configure(std::uint64_t surface_id, std::uint32_t width, std::uint32_t height) const {
    const Impl::SurfaceEntry* e = impl_->surface(surface_id);
    return e != nullptr ? e->surface->configure(width, height) : 0;
}
bool SessionLockManagerShim::lock_surface_on_output(std::uint64_t surface_id, const OutputGlobalShim& output) const {
    const Impl::SurfaceEntry* e = impl_->surface(surface_id);
    return e != nullptr && is_output(e->surface->output_resource(), output);
}

// ---- selections ------------------------------------------------------------

namespace {

// DataDeviceManager and PrimarySelectionManager have the same three hooks.
template <class Manager>
struct SelectionShimImpl {
    std::optional<Manager> mgr;
    std::deque<std::vector<std::string>> changes;
    Conn<lm::SelectionChange> on_change;

    void watch() {
        on_change = mgr->selection_changed().connect(
            [this](lm::SelectionChange& e) { changes.push_back(e.mime_types); });
    }
    bool next(rust::Vec<rust::String>& out) {
        if (changes.empty()) return false;
        out = to_rust(changes.front());
        changes.pop_front();
        return true;
    }
    rust::Vec<rust::String> mime_types() const {
        return mgr ? to_rust(mgr->selection_mime_types()) : rust::Vec<rust::String>{};
    }
    bool receive(rust::Str mime, int fd) {
        if (!mgr) {
            if (fd >= 0) ::close(fd);
            return false;
        }
        return mgr->selection_receive(std::string(mime), fd);
    }
};

} // namespace

struct DataDeviceManagerShim::Impl : SelectionShimImpl<lm::DataDeviceManager> {};
DataDeviceManagerShim::~DataDeviceManagerShim() = default;
DataDeviceManagerShim::DataDeviceManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DataDeviceManagerShim::Impl& DataDeviceManagerShim::impl() const { return *impl_; }
std::unique_ptr<DataDeviceManagerShim> data_device_manager_new(const DisplayShim& display, const SeatShim& seat, rust::String& error) {
    try {
        if (!display.impl().display || !seat.impl().seat) { error = "null display or seat"; return nullptr; }
        auto r = lm::DataDeviceManager::create(*display.impl().display, *seat.impl().seat);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<DataDeviceManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        impl->watch();
        return std::make_unique<DataDeviceManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}
bool DataDeviceManagerShim::next_selection_change(rust::Vec<rust::String>& mime_types) const { return impl_->next(mime_types); }
rust::Vec<rust::String> DataDeviceManagerShim::selection_mime_types() const { return impl_->mime_types(); }
bool DataDeviceManagerShim::selection_receive(rust::Str mime, int fd) const { return impl_->receive(mime, fd); }

struct PrimarySelectionManagerShim::Impl : SelectionShimImpl<lm::PrimarySelectionManager> {};
PrimarySelectionManagerShim::~PrimarySelectionManagerShim() = default;
PrimarySelectionManagerShim::PrimarySelectionManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PrimarySelectionManagerShim::Impl& PrimarySelectionManagerShim::impl() const { return *impl_; }
std::unique_ptr<PrimarySelectionManagerShim> primary_selection_manager_new(const DisplayShim& display, const SeatShim& seat, rust::String& error) {
    try {
        if (!display.impl().display || !seat.impl().seat) { error = "null display or seat"; return nullptr; }
        auto r = lm::PrimarySelectionManager::create(*display.impl().display, *seat.impl().seat);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<PrimarySelectionManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        impl->watch();
        return std::make_unique<PrimarySelectionManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}
bool PrimarySelectionManagerShim::next_selection_change(rust::Vec<rust::String>& mime_types) const { return impl_->next(mime_types); }
rust::Vec<rust::String> PrimarySelectionManagerShim::selection_mime_types() const { return impl_->mime_types(); }
bool PrimarySelectionManagerShim::selection_receive(rust::Str mime, int fd) const { return impl_->receive(mime, fd); }

struct RelativePointerManagerShim::Impl { std::optional<lm::RelativePointerManager> mgr; };
RelativePointerManagerShim::~RelativePointerManagerShim() = default;
RelativePointerManagerShim::RelativePointerManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RelativePointerManagerShim::Impl& RelativePointerManagerShim::impl() const { return *impl_; }
std::unique_ptr<RelativePointerManagerShim> relative_pointer_manager_new(const DisplayShim& display, const SeatShim& seat, rust::String& error) {
    try {
        if (!display.impl().display || !seat.impl().seat) { error = "null display or seat"; return nullptr; }
        auto r = lm::RelativePointerManager::create(*display.impl().display, *seat.impl().seat);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<RelativePointerManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        return std::make_unique<RelativePointerManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}

// ---- pointer-constraints ---------------------------------------------------

struct PointerConstraintsShim::Impl {
    struct Entry {
        lm::PointerConstraint* constraint = nullptr;
        Conn<lm::PointerConstraintDestroy> on_destroy;
        Conn<lm::PointerConstraintRegionChange> on_region;
    };
    std::optional<lm::PointerConstraints> mgr;
    std::unordered_map<std::uint64_t, std::unique_ptr<Entry>> constraints;
    Events events;
    Conn<lm::NewPointerConstraint> on_new;

    lm::PointerConstraint* find(std::uint64_t id) const {
        const auto it = constraints.find(id);
        return it == constraints.end() ? nullptr : it->second->constraint;
    }
};
PointerConstraintsShim::~PointerConstraintsShim() = default;
PointerConstraintsShim::PointerConstraintsShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PointerConstraintsShim::Impl& PointerConstraintsShim::impl() const { return *impl_; }
std::unique_ptr<PointerConstraintsShim> pointer_constraints_new(const DisplayShim& display, const SeatShim& seat, rust::String& error) {
    try {
        if (!display.impl().display || !seat.impl().seat) { error = "null display or seat"; return nullptr; }
        auto r = lm::PointerConstraints::create(*display.impl().display, *seat.impl().seat);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<PointerConstraintsShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        auto* raw = impl.get();
        raw->on_new = raw->mgr->new_constraint().connect([raw](lm::NewPointerConstraint& e) {
            const std::uint64_t id = u64_from_id(e.constraint.surface().id());
            auto entry = std::make_unique<PointerConstraintsShim::Impl::Entry>();
            entry->constraint = &e.constraint;
            entry->on_region = e.constraint.region_change.connect([raw, id](lm::PointerConstraintRegionChange&) {
                raw->events.push(event(EventKind::PointerConstraintRegionChange, id));
            });
            entry->on_destroy = e.constraint.destroy.connect([raw, id](lm::PointerConstraintDestroy&) {
                raw->events.push(event(EventKind::PointerConstraintDestroy, id));
                raw->constraints.erase(id);
            });
            raw->events.push(event(EventKind::NewPointerConstraint, id));
            raw->constraints[id] = std::move(entry);
        });
        return std::make_unique<PointerConstraintsShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}
bool PointerConstraintsShim::next_event(EventRaw& out) const { return impl_->events.pop(out); }
bool PointerConstraintsShim::constraint_info(std::uint64_t surface_id, PointerConstraintInfoRaw& out) const {
    const lm::PointerConstraint* c = impl_->find(surface_id);
    if (c == nullptr) return false;
    out.locked = c->type() == lm::PointerConstraintType::locked;
    out.persistent = c->lifetime() == lm::PointerConstraintLifetime::persistent;
    out.has_region = c->has_region();
    out.has_hint = c->has_cursor_position_hint();
    out.hint_x = c->cursor_hint_x();
    out.hint_y = c->cursor_hint_y();
    out.active = c->active();
    return true;
}
rust::Vec<Rect> PointerConstraintsShim::constraint_region(std::uint64_t surface_id) const {
    rust::Vec<Rect> out;
    if (const lm::PointerConstraint* c = impl_->find(surface_id); c != nullptr) {
        for (const auto& b : c->region().rects()) out.push_back(to_rect(b));
    }
    return out;
}
void PointerConstraintsShim::activate(std::uint64_t surface_id) const {
    if (lm::PointerConstraint* c = impl_->find(surface_id); c != nullptr) c->activate();
}
void PointerConstraintsShim::deactivate(std::uint64_t surface_id) const {
    if (lm::PointerConstraint* c = impl_->find(surface_id); c != nullptr) c->deactivate();
}
std::uint64_t PointerConstraintsShim::active_constraint() const {
    if (!impl_->mgr) return 0;
    const lm::PointerConstraint* c = std::as_const(*impl_->mgr).active_constraint();
    return c != nullptr ? u64_from_id(c->surface().id()) : 0;
}

// ---- text-input-v3 ---------------------------------------------------------

struct TextInputManagerShim::Impl {
    struct Entry {
        lm::TextInput* text_input = nullptr;
        Conn<lm::TextInputEnable> on_enable;
        Conn<lm::TextInputDisable> on_disable;
        Conn<lm::TextInputCommit> on_commit;
        Conn<lm::TextInputDestroy> on_destroy;
    };
    std::optional<lm::TextInputManager> mgr;
    std::unordered_map<std::uint64_t, std::unique_ptr<Entry>> text_inputs;
    Events events;
    Conn<lm::NewTextInput> on_new;

    lm::TextInput* find(std::uint64_t id) const {
        const auto it = text_inputs.find(id);
        return it == text_inputs.end() ? nullptr : it->second->text_input;
    }
};
TextInputManagerShim::~TextInputManagerShim() = default;
TextInputManagerShim::TextInputManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TextInputManagerShim::Impl& TextInputManagerShim::impl() const { return *impl_; }
std::unique_ptr<TextInputManagerShim> text_input_manager_new(const DisplayShim& display, const SeatShim& seat, rust::String& error) {
    try {
        if (!display.impl().display || !seat.impl().seat) { error = "null display or seat"; return nullptr; }
        auto r = lm::TextInputManager::create(*display.impl().display, *seat.impl().seat);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<TextInputManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        auto* raw = impl.get();
        raw->on_new = raw->mgr->new_text_input().connect([raw](lm::NewTextInput& e) {
            const std::uint64_t id = next_object_id();
            auto entry = std::make_unique<TextInputManagerShim::Impl::Entry>();
            entry->text_input = &e.text_input;
            auto plain = [raw, id](EventKind kind) {
                return [raw, id, kind](auto&) { raw->events.push(event(kind, id)); };
            };
            entry->on_enable = e.text_input.enable.connect(plain(EventKind::TextInputEnable));
            entry->on_disable = e.text_input.disable.connect(plain(EventKind::TextInputDisable));
            entry->on_commit = e.text_input.commit.connect(plain(EventKind::TextInputCommit));
            entry->on_destroy = e.text_input.destroy.connect([raw, id](lm::TextInputDestroy&) {
                raw->events.push(event(EventKind::TextInputDestroy, id));
                raw->text_inputs.erase(id);
            });
            raw->events.push(event(EventKind::NewTextInput, id));
            raw->text_inputs[id] = std::move(entry);
        });
        return std::make_unique<TextInputManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}
bool TextInputManagerShim::next_event(EventRaw& out) const { return impl_->events.pop(out); }
std::uint64_t TextInputManagerShim::focused() const {
    if (!impl_->mgr) return 0;
    const lm::TextInput* focused = impl_->mgr->focused();
    for (const auto& [id, entry] : impl_->text_inputs) {
        if (focused != nullptr && entry->text_input == focused) return id;
    }
    return 0;
}
bool TextInputManagerShim::text_input_info(std::uint64_t text_input, TextInputInfoRaw& out) const {
    const lm::TextInput* t = impl_->find(text_input);
    if (t == nullptr) return false;
    out.focused_surface = surface_u64(t->focused_surface());
    out.enabled = t->enabled();
    out.cursor = t->surrounding_cursor();
    out.anchor = t->surrounding_anchor();
    out.change_cause = static_cast<std::uint32_t>(t->text_change_cause());
    out.content_hint = t->content_hint();
    out.content_purpose = static_cast<std::uint32_t>(t->content_purpose());
    out.cursor_rectangle = to_rect(t->cursor_rectangle());
    out.serial = t->serial();
    return true;
}
rust::String TextInputManagerShim::surrounding_text(std::uint64_t text_input) const {
    const lm::TextInput* t = impl_->find(text_input);
    return t != nullptr ? to_rust(t->surrounding_text()) : rust::String{};
}
void TextInputManagerShim::send_preedit_string(std::uint64_t text_input, rust::Str text, std::int32_t cursor_begin, std::int32_t cursor_end) const {
    if (lm::TextInput* t = impl_->find(text_input); t != nullptr) t->send_preedit_string(std::string(text), cursor_begin, cursor_end);
}
void TextInputManagerShim::send_commit_string(std::uint64_t text_input, rust::Str text) const {
    if (lm::TextInput* t = impl_->find(text_input); t != nullptr) t->send_commit_string(std::string(text));
}
void TextInputManagerShim::send_delete_surrounding_text(std::uint64_t text_input, std::uint32_t before, std::uint32_t after) const {
    if (lm::TextInput* t = impl_->find(text_input); t != nullptr) t->send_delete_surrounding_text(before, after);
}
void TextInputManagerShim::send_done(std::uint64_t text_input) const {
    if (lm::TextInput* t = impl_->find(text_input); t != nullptr) t->send_done();
}

// ============================================================================
// GPU & Render
// ============================================================================

struct VulkanRendererShim::Impl {
    std::unique_ptr<lm::VulkanRenderer> renderer;
};
VulkanRendererShim::~VulkanRendererShim() = default;
VulkanRendererShim::VulkanRendererShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
VulkanRendererShim::Impl& VulkanRendererShim::impl() const { return *impl_; }

bool drm_device_id_from_fd(int fd, DrmDeviceId& out) {
    const auto id = lm::drm_device_id(fd);
    if (!id) return false;
    out = DrmDeviceId{id->major, id->minor};
    return true;
}

bool drm_device_id_from_path(rust::Str path, DrmDeviceId& out) {
    const auto id = lm::drm_device_id(std::string(path));
    if (!id) return false;
    out = DrmDeviceId{id->major, id->minor};
    return true;
}

namespace {

lm::VulkanOptions vulkan_options(bool has_drm_device, const DrmDeviceId& drm_device) {
    lm::VulkanOptions opts{};
    if (has_drm_device) opts.drm_device = lm::DrmDeviceId{drm_device.major, drm_device.minor};
    return opts;
}

VulkanHandlesRaw handles_raw(const lm::VulkanRenderer& renderer) {
    const lm::VulkanHandles h = renderer.handles();
    VulkanHandlesRaw out{};
    out.instance = reinterpret_cast<std::uint64_t>(h.instance);
    out.physical_device = reinterpret_cast<std::uint64_t>(h.physical_device);
    out.device = reinterpret_cast<std::uint64_t>(h.device);
    out.queue = reinterpret_cast<std::uint64_t>(h.queue);
    out.queue_family = h.queue_family;
    out.api_version = h.api_version;
    for (const char* ext : h.device_extensions) out.device_extensions.push_back(rust::String(ext));
    return out;
}

} // namespace

std::unique_ptr<VulkanRendererShim> vulkan_renderer_new(bool has_drm_device, const DrmDeviceId& drm_device, rust::String& error) {
    try {
        const lm::VulkanOptions opts = vulkan_options(has_drm_device, drm_device);
        auto r = lm::VulkanRenderer::create(opts);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<VulkanRendererShim::Impl>();
        impl->renderer = std::make_unique<lm::VulkanRenderer>(std::move(*r));
        return std::make_unique<VulkanRendererShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

rust::Vec<std::uint64_t> VulkanRendererShim::scanout_modifiers(std::uint32_t drm_format) const {
    rust::Vec<std::uint64_t> out;
    if (impl_->renderer) {
        for (auto m : impl_->renderer->scanout_modifiers(drm_format)) {
            out.push_back(m);
        }
    }
    return out;
}

VulkanHandlesRaw VulkanRendererShim::handles() const {
    return impl_->renderer ? handles_raw(*impl_->renderer) : VulkanHandlesRaw{};
}

struct LinuxDmabufShim::Impl {
    std::optional<lm::LinuxDmabuf> dmabuf;
};
LinuxDmabufShim::~LinuxDmabufShim() = default;
LinuxDmabufShim::LinuxDmabufShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LinuxDmabufShim::Impl& LinuxDmabufShim::impl() const { return *impl_; }

std::unique_ptr<LinuxDmabufShim> linux_dmabuf_new(const DisplayShim& display, const VulkanRendererShim& renderer, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::LinuxDmabuf::create(*display.impl().display, renderer.impl().renderer.get());
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<LinuxDmabufShim::Impl>();
        impl->dmabuf.emplace(std::move(*r));
        return std::make_unique<LinuxDmabufShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::unique_ptr<LinuxDmabufShim> linux_dmabuf_new_headless(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::LinuxDmabuf::create(*display.impl().display, nullptr);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<LinuxDmabufShim::Impl>();
        impl->dmabuf.emplace(std::move(*r));
        return std::make_unique<LinuxDmabufShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

struct SceneOutputShim::Impl {
    lm::SceneOutput state;
};
SceneOutputShim::~SceneOutputShim() = default;
SceneOutputShim::SceneOutputShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SceneOutputShim::Impl& SceneOutputShim::impl() const { return *impl_; }

std::unique_ptr<SceneOutputShim> scene_output_new() {
    return std::make_unique<SceneOutputShim>(std::make_unique<SceneOutputShim::Impl>());
}
bool SceneOutputShim::gpu_output() const {
    return impl_->state.gpu_output();
}

// The renderer keeps a plain `Presentation*`, and Rust cannot put a lifetime
// on that: the renderer is declared before the Display so that it dies last,
// which makes every global — the Presentation included — die before it. So
// the link is kept here from both ends, and whichever side goes first severs it.
struct SceneRendererShim::Impl {
    std::unique_ptr<lm::SceneRenderer> renderer;
    PresentationShim::Impl* presentation = nullptr;

    void link(PresentationShim::Impl* to);
    ~Impl() { link(nullptr); }
};

struct PresentationShim::Impl {
    std::optional<lm::Presentation> mgr;
    std::vector<SceneRendererShim::Impl*> renderers;

    // Before `mgr` goes: no renderer may still be pointing at it.
    ~Impl() {
        for (SceneRendererShim::Impl* r : renderers) {
            r->presentation = nullptr;
            if (r->renderer) r->renderer->set_presentation(nullptr);
        }
    }
};
PresentationShim::~PresentationShim() = default;
PresentationShim::PresentationShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PresentationShim::Impl& PresentationShim::impl() const { return *impl_; }
std::unique_ptr<PresentationShim> presentation_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::Presentation::create(*display.impl().display);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<PresentationShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        return std::make_unique<PresentationShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}

void SceneRendererShim::Impl::link(PresentationShim::Impl* to) {
    if (presentation != nullptr) std::erase(presentation->renderers, this);
    presentation = to;
    if (to != nullptr) to->renderers.push_back(this);
    if (renderer) renderer->set_presentation(to != nullptr && to->mgr ? &*to->mgr : nullptr);
}

SceneRendererShim::~SceneRendererShim() = default;
SceneRendererShim::SceneRendererShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SceneRendererShim::Impl& SceneRendererShim::impl() const { return *impl_; }

std::unique_ptr<SceneRendererShim> scene_renderer_new(bool has_drm_device, const DrmDeviceId& drm_device, rust::String& error) {
    try {
        const lm::VulkanOptions opts = vulkan_options(has_drm_device, drm_device);
        auto impl = std::make_unique<SceneRendererShim::Impl>();
        impl->renderer = std::make_unique<lm::SceneRenderer>(opts);
        return std::make_unique<SceneRendererShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

bool SceneRendererShim::gpu_enabled() const {
    return impl_->renderer ? impl_->renderer->gpu_enabled() : false;
}
rust::String SceneRendererShim::gpu_error() const {
    return impl_->renderer ? to_rust(std::string(impl_->renderer->gpu_error())) : "not initialized";
}
bool SceneRendererShim::reset(SceneOutputShim& state, OutputShim& output, std::uint32_t drm_format, rust::String& error) const {
    if (!impl_->renderer || !output.impl().output) { error = "null object"; return false; }
    auto st = impl_->renderer->reset(state.impl().state, *output.impl().output, drm_format);
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}
int SceneRendererShim::present(SceneOutputShim& state, OutputShim& output, rust::Slice<const SceneItemRaw> items,
                             float bg_r, float bg_g, float bg_b, float bg_a, rust::String& error) const {
    if (!impl_->renderer || !output.impl().output) { error = "null object"; return -1; }
    const std::vector<lm::SceneItem> scene = scene_items(items);
    auto r = impl_->renderer->present(state.impl().state, *output.impl().output, scene, lm::Color{bg_r, bg_g, bg_b, bg_a});
    if (!r) { error = to_rust(r.error().message); return -1; }
    return static_cast<int>(*r);
}
void SceneRendererShim::set_presentation(const PresentationShim& presentation) const {
    impl_->link(&presentation.impl());
}
void SceneRendererShim::clear_presentation() const {
    impl_->link(nullptr);
}
void SceneRendererShim::update_cursor() const {
    if (impl_->renderer) impl_->renderer->update_cursor();
}
rust::Vec<std::uint8_t> SceneRendererShim::read_back(SceneOutputShim& state, rust::String& error) const {
    rust::Vec<std::uint8_t> out;
    if (!impl_->renderer) { error = "not initialized"; return out; }
    auto r = impl_->renderer->read_back(state.impl().state);
    if (!r) { error = to_rust(r.error().message); return out; }
    const auto& span = *r;
    const auto* ptr = reinterpret_cast<const uint8_t*>(span.data());
    size_t bytes = span.size() * sizeof(lm::Pixel);
    out.reserve(bytes);
    for (size_t i = 0; i < bytes; ++i) out.push_back(ptr[i]);
    return out;
}

bool SceneRendererShim::vulkan_handles(VulkanHandlesRaw& out) const {
    if (!impl_->renderer || impl_->renderer->gpu() == nullptr) return false;
    out = handles_raw(*impl_->renderer->gpu());
    return true;
}

struct OutputLayoutShim::Impl {
    // An output that dies leaves the layout at once. That is not policy: C++
    // compositors remove it in their Output::destroy handler, while Rust only
    // hears of it after dispatch() returns, and the Cursor may read the layout
    // before then.
    struct Watch {
        lm::Output* output;
        Conn<lm::OutputDestroy> on_destroy;
    };

    lm::OutputLayout layout;
    std::deque<Watch> watches;

    void watch(lm::Output* out) {
        if (std::ranges::any_of(watches, [out](const Watch& w) { return w.output == out; })) {
            return;
        }
        // Erasing the watch inside its own slot is safe: a Signal never frees
        // the slot that is running.
        watches.push_back(Watch{out, out->destroy.connect([this, out](lm::OutputDestroy&) {
                                    layout.remove(*out);
                                    unwatch(out);
                                })});
    }
    void unwatch(const lm::Output* output) {
        std::erase_if(watches, [output](const Watch& w) { return w.output == output; });
    }
};
OutputLayoutShim::~OutputLayoutShim() = default;
OutputLayoutShim::OutputLayoutShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
OutputLayoutShim::Impl& OutputLayoutShim::impl() const { return *impl_; }

std::unique_ptr<OutputLayoutShim> output_layout_new() {
    return std::make_unique<OutputLayoutShim>(std::make_unique<OutputLayoutShim::Impl>());
}
void OutputLayoutShim::add(OutputShim& output, std::int32_t x, std::int32_t y) const {
    lm::Output* out = output.impl().output;
    if (!out) return;
    impl_->layout.add(*out, x, y);
    impl_->watch(out);
}
void OutputLayoutShim::add_auto(OutputShim& output) const {
    lm::Output* out = output.impl().output;
    if (!out) return;
    impl_->layout.add_auto(*out);
    impl_->watch(out);
}
Rect OutputLayoutShim::bounds() const {
    const auto b = impl_->layout.bounds();
    return Rect{b.x, b.y, b.width, b.height};
}
void OutputLayoutShim::remove(OutputShim& output) const {
    lm::Output* out = output.impl().output;
    if (!out) return;
    impl_->layout.remove(*out);
    impl_->unwatch(out);
}
Rect OutputLayoutShim::output_box(const OutputShim& output) const {
    if (!output.impl().output) return Rect{0, 0, 0, 0};
    const auto b = impl_->layout.box_of(*output.impl().output);
    return Rect{b.x, b.y, b.width, b.height};
}

struct CursorShim::Impl {
    std::unique_ptr<lm::Cursor> cursor;
};
CursorShim::~CursorShim() = default;
CursorShim::CursorShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CursorShim::Impl& CursorShim::impl() const { return *impl_; }

std::unique_ptr<CursorShim> cursor_new(const SceneRendererShim& renderer, const OutputLayoutShim& layout,
                                      const SeatShim& seat, rust::String& error) {
    try {
        if (!renderer.impl().renderer || !seat.impl().seat) { error = "null renderer or seat"; return nullptr; }
        auto impl = std::make_unique<CursorShim::Impl>();
        impl->cursor = std::make_unique<lm::Cursor>(
            *renderer.impl().renderer, layout.impl().layout, *seat.impl().seat,
            nullptr, nullptr
        );
        return std::make_unique<CursorShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

bool CursorShim::move_to(double x, double y, Rect bounds) {
    return impl_->cursor ? impl_->cursor->move_to(x, y, lm::Box{bounds.x, bounds.y, bounds.width, bounds.height}) : false;
}
bool CursorShim::move_by(double dx, double dy, Rect bounds) {
    return impl_->cursor ? impl_->cursor->move_by(dx, dy, lm::Box{bounds.x, bounds.y, bounds.width, bounds.height}) : false;
}
void CursorShim::leave() {
    if (impl_->cursor) impl_->cursor->leave();
}
void CursorShim::refresh() {
    if (impl_->cursor) impl_->cursor->refresh();
}
bool CursorShim::position(double& x, double& y) const {
    if (!impl_->cursor || !impl_->cursor->pointer().present()) return false;
    x = impl_->cursor->pointer().x();
    y = impl_->cursor->pointer().y();
    return true;
}

struct FrameShim::Impl {
    std::unique_ptr<lm::Frame> frame;
    // lm::Frame keeps an Output&, and the output is its backend's: a monitor
    // unplugged, or the backend dropped, with this frame still in Rust's hands
    // would leave it dangling. No Rust lifetime can say "until hotplug", so the
    // frame goes with the output, as SceneOutput's does, and the shim is inert.
    Conn<lm::OutputDestroy> output_gone;
};
FrameShim::~FrameShim() = default;
FrameShim::FrameShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
FrameShim::Impl& FrameShim::impl() const { return *impl_; }

std::unique_ptr<FrameShim> frame_new(OutputShim& output, const VulkanRendererShim& renderer, rust::String& error) {
    if (!output.impl().output || !renderer.impl().renderer) {
        error = "null output or renderer";
        return nullptr;
    }
    auto impl = std::make_unique<FrameShim::Impl>();
    impl->frame = std::make_unique<lm::Frame>(*output.impl().output, *renderer.impl().renderer);
    impl->output_gone = output.impl().output->destroy.connect([raw = impl.get()](lm::OutputDestroy&) {
        raw->frame.reset();
    });
    return std::make_unique<FrameShim>(std::move(impl));
}
bool FrameShim::reset(std::uint32_t drm_format, rust::String& error) {
    if (!impl_->frame) { error = "frame null"; return false; }
    auto st = impl_->frame->reset(drm_format);
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}
void FrameShim::begin(std::int32_t vx, std::int32_t vy, std::int32_t vw, std::int32_t vh) {
    if (impl_->frame) impl_->frame->begin(lm::Box{vx, vy, vw, vh});
}
void FrameShim::place_rect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                          float r, float g, float b, float a) {
    if (impl_->frame) impl_->frame->place_rect(x, y, width, height, lm::Color{r, g, b, a});
}
int FrameShim::submit(float bg_r, float bg_g, float bg_b, float bg_a, rust::String& error) {
    if (!impl_->frame) { error = "frame null"; return -1; }
    auto r = impl_->frame->submit(lm::Color{bg_r, bg_g, bg_b, bg_a});
    if (!r) { error = to_rust(r.error().message); return -1; }
    return static_cast<int>(*r);
}
void FrameShim::send_frame_done(std::uint32_t time_ms) {
    if (impl_->frame) impl_->frame->send_frame_done(time_ms);
}

struct CpuCompositorShim::Impl {
    lm::CpuCompositor comp;
    std::vector<lm::CpuItem> items;
};
CpuCompositorShim::~CpuCompositorShim() = default;
CpuCompositorShim::CpuCompositorShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CpuCompositorShim::Impl& CpuCompositorShim::impl() const { return *impl_; }

std::unique_ptr<CpuCompositorShim> cpu_compositor_new() {
    return std::make_unique<CpuCompositorShim>(std::make_unique<CpuCompositorShim::Impl>());
}
void CpuCompositorShim::composite_rect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                                      float r, float g, float b, float a) {
    impl_->items.push_back(lm::RectFill{lm::Box{x, y, width, height}, lm::Color{r, g, b, a}});
}
rust::Vec<std::uint8_t> CpuCompositorShim::finish(std::int32_t width, std::int32_t height,
                                                float bg_r, float bg_g, float bg_b, float bg_a) {
    rust::Vec<std::uint8_t> out;
    impl_->comp.composite(width, height, lm::Color{bg_r, bg_g, bg_b, bg_a}, impl_->items);
    impl_->items.clear();
    const auto& px = impl_->comp.pixels();
    const auto* ptr = reinterpret_cast<const uint8_t*>(px.data());
    size_t bytes = px.size() * sizeof(lm::Pixel);
    out.reserve(bytes);
    for (size_t i = 0; i < bytes; ++i) out.push_back(ptr[i]);
    return out;
}

struct DirectScanoutShim::Impl {
    std::unique_ptr<lm::DirectScanout> ds;
};
DirectScanoutShim::~DirectScanoutShim() = default;
DirectScanoutShim::DirectScanoutShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DirectScanoutShim::Impl& DirectScanoutShim::impl() const { return *impl_; }

std::unique_ptr<DirectScanoutShim> direct_scanout_new(OutputShim& output) {
    if (!output.impl().output) return nullptr;
    auto impl = std::make_unique<DirectScanoutShim::Impl>();
    impl->ds = std::make_unique<lm::DirectScanout>(*output.impl().output);
    return std::make_unique<DirectScanoutShim>(std::move(impl));
}
bool DirectScanoutShim::available() const {
    return impl_->ds ? impl_->ds->available() : false;
}
std::uint32_t DirectScanoutShim::id_for(std::uint64_t surface_id) {
    if (!impl_->ds) return 0;
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    if (!s) return 0;
    auto opt = impl_->ds->id_for(*s);
    return opt.value_or(0);
}
void DirectScanoutShim::committed(std::uint64_t surface_id) {
    if (!impl_->ds) return;
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    if (s) impl_->ds->committed(*s);
}
void DirectScanoutShim::presented() {
    if (impl_->ds) impl_->ds->presented();
}
void DirectScanoutShim::clear() {
    if (impl_->ds) impl_->ds->clear();
}
std::size_t DirectScanoutShim::cached() const {
    return impl_->ds ? impl_->ds->cached() : 0;
}

SceneHitRaw raw_scene_hit_test(rust::Slice<const SceneItemRaw> items, double x, double y) {
    const std::vector<lm::SceneItem> scene = scene_items(items);
    auto hit = lm::scene_hit_test(scene, x, y);
    SceneHitRaw out{};
    out.hit = hit.hit;
    out.tag = hit.tag;
    out.surface_id = hit.surface ? u64_from_id(hit.surface->id()) : 0;
    out.sx = hit.sx;
    out.sy = hit.sy;
    return out;
}

// ============================================================================
// Desktop Protocols
// ============================================================================

void foreign_toplevel_track(ForeignToplevelManagerShim& ftm, const XdgShellShim& shell) {
    if (ftm.impl().mgr && shell.impl().shell) {
        ftm.impl().mgr->track(*shell.impl().shell);
    }
}

std::uint32_t workspace_manager_create_group(WorkspaceManagerShim& wm) {
    return wm.impl().mgr ? wm.impl().mgr->add_group() : 0;
}
std::uint32_t workspace_manager_create_workspace(WorkspaceManagerShim& wm, std::uint32_t group, rust::Str name) {
    return wm.impl().mgr ? wm.impl().mgr->add_workspace(group, std::string(name), std::string(name)) : 0;
}
void workspace_manager_remove_workspace(WorkspaceManagerShim& wm, std::uint32_t workspace) {
    if (wm.impl().mgr) wm.impl().mgr->remove_workspace(workspace);
}
void workspace_manager_activate_workspace(WorkspaceManagerShim& wm, std::uint32_t workspace) {
    if (wm.impl().mgr) wm.impl().mgr->activate(workspace);
}

struct OutputManagerShim::Impl {
    std::optional<lm::OutputManager> mgr;
};
OutputManagerShim::~OutputManagerShim() = default;
OutputManagerShim::OutputManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
OutputManagerShim::Impl& OutputManagerShim::impl() const { return *impl_; }

std::unique_ptr<OutputManagerShim> output_manager_new(const DisplayShim& display, const OutputLayoutShim& layout, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::OutputManager::create(*display.impl().display, layout.impl().layout);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<OutputManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        return std::make_unique<OutputManagerShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}
void OutputManagerShim::update() {
    if (impl_->mgr) impl_->mgr->update();
}

struct DataControlManagerShim::Impl {
    std::optional<lm::DataControlManager> mgr;
};
DataControlManagerShim::~DataControlManagerShim() = default;
DataControlManagerShim::DataControlManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DataControlManagerShim::Impl& DataControlManagerShim::impl() const { return *impl_; }

std::unique_ptr<DataControlManagerShim> data_control_manager_new(const DisplayShim& display, const DataDeviceManagerShim& ddm, rust::String& error) {
    try {
        if (!display.impl().display || !ddm.impl().mgr) { error = "null display or data_device"; return nullptr; }
        auto r = lm::DataControlManager::create(*display.impl().display, *ddm.impl().mgr, nullptr);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<DataControlManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        return std::make_unique<DataControlManagerShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

struct InputMethodManagerShim::Impl {
    struct Entry {
        lm::InputMethod* input_method = nullptr;
        Conn<lm::NewInputPopupSurface> on_popup;
        Conn<lm::InputMethodGrabChange> on_grab;
        Conn<lm::InputMethodDestroy> on_destroy;
    };
    struct PopupEntry {
        lm::InputPopupSurface* popup = nullptr;
        Conn<lm::InputPopupSurfaceDestroy> on_destroy;
    };

    std::optional<lm::InputMethodManager> mgr;
    std::unordered_map<std::uint64_t, std::unique_ptr<Entry>> input_methods;
    std::unordered_map<std::uint64_t, std::unique_ptr<PopupEntry>> popups;
    Events events;
    Conn<lm::NewInputMethod> on_new;

    lm::InputMethod* find(std::uint64_t id) const {
        const auto it = input_methods.find(id);
        return it == input_methods.end() ? nullptr : it->second->input_method;
    }

    void watch(lm::InputPopupSurface& popup, std::uint64_t im) {
        const std::uint64_t id = u64_from_id(popup.surface().id());
        auto entry = std::make_unique<PopupEntry>();
        entry->popup = &popup;
        entry->on_destroy = popup.destroy.connect([this, id](lm::InputPopupSurfaceDestroy&) {
            events.push(event(EventKind::InputPopupSurfaceDestroy, id));
            popups.erase(id);
        });
        events.push(event(EventKind::NewInputPopupSurface, im, id));
        popups[id] = std::move(entry);
    }

    void watch(lm::InputMethod& im) {
        const std::uint64_t id = next_object_id();
        auto entry = std::make_unique<Entry>();
        entry->input_method = &im;
        entry->on_popup = im.new_popup_surface.connect(
            [this, id](lm::NewInputPopupSurface& e) { watch(e.popup, id); });
        entry->on_grab = im.grab_changed.connect([this, id](lm::InputMethodGrabChange& e) {
            EventRaw ev = event(EventKind::InputMethodGrabChange, id);
            ev.flag = e.grabbed;
            events.push(std::move(ev));
        });
        entry->on_destroy = im.destroy.connect([this, id](lm::InputMethodDestroy&) {
            events.push(event(EventKind::InputMethodDestroy, id));
            input_methods.erase(id);
        });
        events.push(event(EventKind::NewInputMethod, id));
        input_methods[id] = std::move(entry);
    }
};
InputMethodManagerShim::~InputMethodManagerShim() = default;
InputMethodManagerShim::InputMethodManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
InputMethodManagerShim::Impl& InputMethodManagerShim::impl() const { return *impl_; }

std::unique_ptr<InputMethodManagerShim> input_method_manager_new(const DisplayShim& display, const SeatShim& seat, const TextInputManagerShim& tim, rust::String& error) {
    try {
        if (!display.impl().display || !seat.impl().seat || !tim.impl().mgr) {
            error = "null display, seat or text_inputs";
            return nullptr;
        }
        auto r = lm::InputMethodManager::create(*display.impl().display, *seat.impl().seat, *tim.impl().mgr);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<InputMethodManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        auto* raw = impl.get();
        raw->on_new = raw->mgr->new_input_method().connect(
            [raw](lm::NewInputMethod& e) { raw->watch(e.input_method); });
        return std::make_unique<InputMethodManagerShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}
bool InputMethodManagerShim::next_event(EventRaw& out) const { return impl_->events.pop(out); }
std::uint64_t InputMethodManagerShim::current() const {
    if (!impl_->mgr) return 0;
    const lm::InputMethod* current = impl_->mgr->current();
    for (const auto& [id, entry] : impl_->input_methods) {
        if (current != nullptr && entry->input_method == current) return id;
    }
    return 0;
}
bool InputMethodManagerShim::active(std::uint64_t input_method) const {
    const lm::InputMethod* im = impl_->find(input_method);
    return im != nullptr && im->active();
}
bool InputMethodManagerShim::has_keyboard_grab(std::uint64_t input_method) const {
    lm::InputMethod* im = impl_->find(input_method);
    return im != nullptr && im->keyboard_grab() != nullptr;
}
void InputMethodManagerShim::grab_send_key(std::uint64_t input_method, std::uint32_t time, std::uint32_t key, bool pressed) const {
    if (lm::InputMethod* im = impl_->find(input_method); im != nullptr && im->keyboard_grab() != nullptr) {
        im->keyboard_grab()->send_key(time, key, pressed);
    }
}
void InputMethodManagerShim::grab_send_modifiers(std::uint64_t input_method, std::uint32_t depressed, std::uint32_t latched,
                                                 std::uint32_t locked, std::uint32_t group) const {
    if (lm::InputMethod* im = impl_->find(input_method); im != nullptr && im->keyboard_grab() != nullptr) {
        im->keyboard_grab()->send_modifiers(depressed, latched, locked, group);
    }
}
void InputMethodManagerShim::grab_send_repeat_info(std::uint64_t input_method, std::int32_t rate, std::int32_t delay) const {
    if (lm::InputMethod* im = impl_->find(input_method); im != nullptr && im->keyboard_grab() != nullptr) {
        im->keyboard_grab()->send_repeat_info(rate, delay);
    }
}
rust::Vec<std::uint64_t> InputMethodManagerShim::popup_surfaces(std::uint64_t input_method) const {
    rust::Vec<std::uint64_t> out;
    if (const lm::InputMethod* im = impl_->find(input_method); im != nullptr) {
        for (lm::InputPopupSurface* p : im->popup_surfaces()) out.push_back(u64_from_id(p->surface().id()));
    }
    return out;
}
Rect InputMethodManagerShim::popup_text_input_rectangle(std::uint64_t surface_id) const {
    const auto it = impl_->popups.find(surface_id);
    return it == impl_->popups.end() ? Rect{0, 0, 0, 0} : to_rect(it->second->popup->text_input_rectangle());
}

struct DesktopGlobalsShim::Impl {
    std::optional<lm::DesktopGlobals> globals;
};
DesktopGlobalsShim::~DesktopGlobalsShim() = default;
DesktopGlobalsShim::DesktopGlobalsShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DesktopGlobalsShim::Impl& DesktopGlobalsShim::impl() const { return *impl_; }

std::unique_ptr<DesktopGlobalsShim> desktop_globals_new(const DisplayShim& display, const VulkanRendererShim& renderer, rust::Str seat_name, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::DesktopGlobals::create(*display.impl().display, renderer.impl().renderer.get(), std::string(seat_name));
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<DesktopGlobalsShim::Impl>();
        impl->globals.emplace(std::move(*r));
        return std::make_unique<DesktopGlobalsShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::unique_ptr<DesktopGlobalsShim> desktop_globals_new_headless(const DisplayShim& display, rust::Str seat_name, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::DesktopGlobals::create(*display.impl().display, nullptr, std::string(seat_name));
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<DesktopGlobalsShim::Impl>();
        impl->globals.emplace(std::move(*r));
        return std::make_unique<DesktopGlobalsShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

// lm::PopupTree holds each popup by reference, and the compositor drops it in
// the popup's destroy handler. Rust only hears of the destroy after dispatch()
// returns, so the shim watches each popup it was given and drops it itself.
struct PopupTreeShim::Impl {
    struct Watch {
        std::uint64_t id;
        lm::Popup* popup;
        Conn<lm::PopupDestroy> on_destroy;
    };
    lm::PopupTree tree;
    std::vector<Watch> watches;

    lm::Popup* find(std::uint64_t id) const {
        const auto it = std::ranges::find(watches, id, &Watch::id);
        return it == watches.end() ? nullptr : it->popup;
    }
    void forget(std::uint64_t id) {
        if (lm::Popup* p = find(id); p != nullptr) tree.remove(*p);
        // Erasing the watch inside its own slot is safe: a Signal never frees
        // the slot that is running.
        std::erase_if(watches, [id](const Watch& w) { return w.id == id; });
    }
};
PopupTreeShim::~PopupTreeShim() = default;
PopupTreeShim::PopupTreeShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PopupTreeShim::Impl& PopupTreeShim::impl() const { return *impl_; }

std::unique_ptr<PopupTreeShim> popup_tree_new() {
    return std::make_unique<PopupTreeShim>(std::make_unique<PopupTreeShim::Impl>());
}
bool PopupTreeShim::add(const XdgShellShim& shell, std::uint64_t popup, std::uint64_t parent) {
    lm::Popup* p = shell.impl().popup(popup);
    if (p == nullptr) return false;
    impl_->tree.add(*p, id_from_u64(parent));
    if (impl_->find(popup) == nullptr) {
        Impl* raw = impl_.get();
        impl_->watches.push_back(Impl::Watch{popup, p, p->destroy.connect([raw, popup](lm::PopupDestroy&) {
                                                 raw->forget(popup);
                                             })});
    }
    return true;
}
void PopupTreeShim::set_mapped(std::uint64_t popup, bool mapped) {
    if (lm::Popup* p = impl_->find(popup); p != nullptr) impl_->tree.set_mapped(*p, mapped);
}
void PopupTreeShim::remove(std::uint64_t popup) { impl_->forget(popup); }
rust::Vec<SceneItemRaw> PopupTreeShim::append_to(rust::Slice<const SceneItemRaw> scene) const {
    std::vector<lm::SceneItem> items = scene_items(scene);
    const std::size_t before = items.size();
    impl_->tree.append_to(items);
    rust::Vec<SceneItemRaw> out;
    for (std::size_t i = before; i < items.size(); ++i) out.push_back(scene_item_raw(items[i]));
    return out;
}
bool PopupTreeShim::contains(std::uint64_t surface_id) const {
    return impl_->tree.contains(id_from_u64(surface_id));
}
std::uint64_t PopupTreeShim::active_grab() const {
    const lm::Popup* p = impl_->tree.active_grab();
    return p != nullptr ? u64_from_id(const_cast<lm::Popup*>(p)->surface().id()) : 0;
}
bool PopupTreeShim::dismiss_grabs_outside(std::uint64_t surface_id) {
    return impl_->tree.dismiss_grabs_outside(id_from_u64(surface_id));
}

// Same reason as PopupTreeShim: every layer surface handed in is watched, and
// leaves the manager by itself when it is destroyed.
struct LayerManagerShim::Impl {
    struct Watch {
        std::uint64_t id;
        lm::LayerSurface* surface;
        Conn<lm::LayerSurfaceDestroy> on_destroy;
    };
    lm::LayerManager mgr;
    std::vector<Watch> watches;

    lm::LayerSurface* find(std::uint64_t id) const {
        const auto it = std::ranges::find(watches, id, &Watch::id);
        return it == watches.end() ? nullptr : it->surface;
    }
    void forget(std::uint64_t id) {
        if (lm::LayerSurface* ls = find(id); ls != nullptr) mgr.remove(*ls);
        std::erase_if(watches, [id](const Watch& w) { return w.id == id; });
    }
};
LayerManagerShim::~LayerManagerShim() = default;
LayerManagerShim::LayerManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LayerManagerShim::Impl& LayerManagerShim::impl() const { return *impl_; }

std::unique_ptr<LayerManagerShim> layer_manager_new() {
    return std::make_unique<LayerManagerShim>(std::make_unique<LayerManagerShim::Impl>());
}
bool LayerManagerShim::add(const LayerShellShim& shell, std::uint64_t surface_id, std::uint32_t output_id) {
    lm::LayerSurface* ls = shell.impl().find(surface_id);
    if (ls == nullptr) return false;
    impl_->mgr.add(*ls, output_id);
    if (impl_->find(surface_id) == nullptr) {
        Impl* raw = impl_.get();
        impl_->watches.push_back(Impl::Watch{surface_id, ls, ls->destroy.connect([raw, surface_id](lm::LayerSurfaceDestroy&) {
                                                 raw->forget(surface_id);
                                             })});
    }
    return true;
}
void LayerManagerShim::remove(std::uint64_t surface_id) { impl_->forget(surface_id); }
void LayerManagerShim::set_mapped(std::uint64_t surface_id, bool mapped) {
    if (lm::LayerSurface* ls = impl_->find(surface_id); ls != nullptr) impl_->mgr.set_mapped(*ls, mapped);
}
Rect LayerManagerShim::arrange(std::uint32_t output_id, Rect full, Rect usable) {
    return to_rect(impl_->mgr.arrange(output_id, to_box(full), to_box(usable)));
}
rust::Vec<SceneItemRaw> LayerManagerShim::append_to(std::uint32_t output_id, std::uint32_t layer) const {
    std::vector<lm::SceneItem> items;
    impl_->mgr.append_to(items, output_id, static_cast<lm::Layer>(layer));
    rust::Vec<SceneItemRaw> out;
    for (const lm::SceneItem& it : items) out.push_back(scene_item_raw(it));
    return out;
}
std::uint64_t LayerManagerShim::keyboard_target(std::uint64_t surface_id) const {
    lm::LayerSurface* ls = impl_->mgr.keyboard_target(id_from_u64(surface_id));
    return ls != nullptr ? u64_from_id(ls->surface().id()) : 0;
}
void LayerManagerShim::close_output(std::uint32_t output_id) { impl_->mgr.close_output(output_id); }

} // namespace lum
