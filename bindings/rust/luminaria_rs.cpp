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
#include <wayland-server-core.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
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

bool SessionShim::switch_to(int vt, rust::String& error) {
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

std::uint32_t SessionShim::next_activity() {
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

inline lm::SurfaceId id_from_u64(std::uint64_t raw) {
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

struct XdgShellShim::Impl {
    std::optional<lm::XdgShell> shell;
    struct ToplevelEntry {
        lm::Toplevel* toplevel = nullptr;
        std::uint64_t surface_id = 0;
        Conn<lm::ToplevelMap> on_map;
        Conn<lm::ToplevelUnmap> on_unmap;
        Conn<lm::ToplevelDestroy> on_destroy;
    };
    std::vector<std::unique_ptr<ToplevelEntry>> toplevels;
    std::deque<ToplevelEventRaw> events;
    Conn<lm::NewToplevel> on_new_toplevel;
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
        auto* raw_impl = impl.get();
        raw_impl->on_new_toplevel = raw_impl->shell->new_toplevel().connect([raw_impl](lm::NewToplevel& e) {
            auto entry = std::make_unique<XdgShellShim::Impl::ToplevelEntry>();
            entry->toplevel = &e.toplevel;
            entry->surface_id = u64_from_id(e.toplevel.surface().id());
            raw_impl->events.push_back(ToplevelEventRaw{0 /* Created */, entry->surface_id});

            auto* p_entry = entry.get();
            entry->on_map = e.toplevel.map.connect([raw_impl, p_entry](lm::ToplevelMap&) {
                raw_impl->events.push_back(ToplevelEventRaw{1 /* Map */, p_entry->surface_id});
            });
            entry->on_unmap = e.toplevel.unmap.connect([raw_impl, p_entry](lm::ToplevelUnmap&) {
                raw_impl->events.push_back(ToplevelEventRaw{2 /* Unmap */, p_entry->surface_id});
            });
            entry->on_destroy = e.toplevel.destroy.connect([raw_impl, p_entry](lm::ToplevelDestroy&) {
                raw_impl->events.push_back(ToplevelEventRaw{3 /* Destroy */, p_entry->surface_id});
                for (auto it = raw_impl->toplevels.begin(); it != raw_impl->toplevels.end(); ++it) {
                    if (it->get() == p_entry) {
                        raw_impl->toplevels.erase(it);
                        break;
                    }
                }
            });
            raw_impl->toplevels.push_back(std::move(entry));
        });
        return std::make_unique<XdgShellShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

void XdgShellShim::set_bounds(std::int32_t width, std::int32_t height) {
    if (impl_->shell) impl_->shell->set_bounds(width, height);
}

bool XdgShellShim::next_toplevel_event(ToplevelEventRaw& out) {
    if (impl_->events.empty()) return false;
    out = impl_->events.front();
    impl_->events.pop_front();
    return true;
}

void XdgShellShim::toplevel_configure(std::uint64_t surface_id, std::int32_t width, std::int32_t height) {
    for (const auto& entry : impl_->toplevels) {
        if (entry->toplevel && u64_from_id(entry->toplevel->surface().id()) == surface_id) {
            (void)entry->toplevel->configure(width, height);
            return;
        }
    }
}

void XdgShellShim::toplevel_set_maximized(std::uint64_t surface_id, bool on) {
    for (const auto& entry : impl_->toplevels) {
        if (entry->toplevel && u64_from_id(entry->toplevel->surface().id()) == surface_id) {
            entry->toplevel->set_maximized(on);
            return;
        }
    }
}

void XdgShellShim::toplevel_set_activated(std::uint64_t surface_id, bool on) {
    for (const auto& entry : impl_->toplevels) {
        if (entry->toplevel && u64_from_id(entry->toplevel->surface().id()) == surface_id) {
            entry->toplevel->set_activated(on);
            return;
        }
    }
}

void XdgShellShim::toplevel_close(std::uint64_t surface_id) {
    for (const auto& entry : impl_->toplevels) {
        if (entry->toplevel && u64_from_id(entry->toplevel->surface().id()) == surface_id) {
            entry->toplevel->close();
            return;
        }
    }
}

struct SeatShim::Impl {
    std::optional<lm::Seat> seat;
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
        return std::make_unique<SeatShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

void SeatShim::set_capabilities(bool keyboard, bool pointer, bool touch) {
    if (impl_->seat) impl_->seat->set_capabilities(keyboard, pointer, touch);
}
bool SeatShim::set_keymap(rust::Str xkb_text) {
    if (!impl_->seat) return false;
    return impl_->seat->set_keymap(std::string(xkb_text));
}
rust::String SeatShim::keymap() const {
    if (!impl_->seat) return "";
    return to_rust(impl_->seat->keymap());
}
void SeatShim::set_keyboard_focus(std::uint64_t surface_id) {
    if (!impl_->seat) return;
    lm::Surface* s = surface_id != 0 ? lm::surface_from_id(id_from_u64(surface_id)) : nullptr;
    impl_->seat->set_keyboard_focus(s);
}
std::uint64_t SeatShim::keyboard_focus() const {
    if (!impl_->seat) return 0;
    return u64_from_id(impl_->seat->keyboard_focus());
}
void SeatShim::notify_key(std::uint32_t key, bool pressed) {
    if (impl_->seat) impl_->seat->notify_key(key, pressed);
}
void SeatShim::notify_modifiers(std::uint32_t depressed, std::uint32_t latched,
                               std::uint32_t locked, std::uint32_t group) {
    if (impl_->seat) impl_->seat->notify_modifiers(depressed, latched, locked, group);
}
void SeatShim::pointer_enter(std::uint64_t surface_id, double sx, double sy) {
    if (!impl_->seat) return;
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    if (s) impl_->seat->pointer_enter(*s, sx, sy);
}
void SeatShim::pointer_clear_focus() {
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
void SeatShim::pointer_motion(double sx, double sy) {
    if (impl_->seat) impl_->seat->pointer_motion(sx, sy);
}
void SeatShim::pointer_button(std::uint32_t button, bool pressed) {
    if (impl_->seat) impl_->seat->pointer_button(button, pressed);
}
void SeatShim::pointer_axis(double dx, double dy) {
    if (impl_->seat) impl_->seat->pointer_axis(dx, dy);
}
void SeatShim::pointer_axis_discrete(std::int32_t dx_steps, std::int32_t dy_steps) {
    if (impl_->seat) impl_->seat->pointer_axis_discrete(dx_steps, dy_steps);
}
void SeatShim::pointer_axis_stop(bool horizontal, bool vertical) {
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
void SeatShim::touch_down(std::uint64_t surface_id, std::int32_t id, double x, double y) {
    if (!impl_->seat) return;
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    if (s) impl_->seat->touch_down(*s, id, x, y);
}
void SeatShim::touch_motion(std::int32_t id, double x, double y) {
    if (impl_->seat) impl_->seat->touch_motion(id, x, y);
}
void SeatShim::touch_up(std::int32_t id) {
    if (impl_->seat) impl_->seat->touch_up(id);
}
void SeatShim::touch_frame() {
    if (impl_->seat) impl_->seat->touch_frame();
}
void SeatShim::touch_cancel() {
    if (impl_->seat) impl_->seat->touch_cancel();
}
std::uint64_t SeatShim::touch_focus(std::int32_t id) const {
    if (!impl_->seat) return 0;
    return u64_from_id(impl_->seat->touch_focus(id));
}

struct LayerShellShim::Impl {
    std::optional<lm::LayerShell> layer_shell;
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
        return std::make_unique<LayerShellShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
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
DEFINE_GLOBAL_SHIM_IMPL(XdgActivation, XdgActivation, xdg_activation_new)
DEFINE_GLOBAL_SHIM_IMPL(XdgDecorationManager, XdgDecorationManager, xdg_decoration_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(CursorShapeManager, CursorShapeManager, cursor_shape_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(IdleInhibitManager, IdleInhibitManager, idle_inhibit_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(SinglePixelBufferManager, SinglePixelBufferManager, single_pixel_buffer_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(TearingControlManager, TearingControlManager, tearing_control_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(ContentTypeManager, ContentTypeManager, content_type_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(FifoManager, FifoManager, fifo_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(CommitTimingManager, CommitTimingManager, commit_timing_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(Presentation, Presentation, presentation_new)
DEFINE_GLOBAL_SHIM_IMPL(BackgroundEffectManager, BackgroundEffectManager, background_effect_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(XdgToplevelIconManager, XdgToplevelIconManager, xdg_toplevel_icon_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(DrmSyncobjManager, DrmSyncobjManager, drm_syncobj_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(ScreencopyManager, ScreencopyManager, screencopy_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(ForeignToplevelManager, ForeignToplevelManager, foreign_toplevel_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(WorkspaceManager, WorkspaceManager, workspace_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(SessionLockManager, SessionLockManager, session_lock_manager_new)

#undef DEFINE_GLOBAL_SHIM_IMPL

struct IdleNotifierShim::Impl { std::optional<lm::IdleNotifier> mgr; };
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
        return std::make_unique<IdleNotifierShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}

struct DataDeviceManagerShim::Impl { std::optional<lm::DataDeviceManager> mgr; };
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
        return std::make_unique<DataDeviceManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}

struct PrimarySelectionManagerShim::Impl { std::optional<lm::PrimarySelectionManager> mgr; };
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
        return std::make_unique<PrimarySelectionManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}

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

struct PointerConstraintsShim::Impl {
    std::optional<lm::PointerConstraints> mgr;
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
        return std::make_unique<PointerConstraintsShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}

struct TextInputManagerShim::Impl {
    std::optional<lm::TextInputManager> mgr;
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
        return std::make_unique<TextInputManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
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

struct SceneRendererShim::Impl {
    std::unique_ptr<lm::SceneRenderer> renderer;
};
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
bool SceneRendererShim::reset(SceneOutputShim& state, OutputShim& output, std::uint32_t drm_format, rust::String& error) {
    if (!impl_->renderer || !output.impl().output) { error = "null object"; return false; }
    auto st = impl_->renderer->reset(state.impl().state, *output.impl().output, drm_format);
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}
int SceneRendererShim::present(SceneOutputShim& state, OutputShim& output, rust::Slice<const SceneItemRaw> items,
                             float bg_r, float bg_g, float bg_b, float bg_a, rust::String& error) {
    if (!impl_->renderer || !output.impl().output) { error = "null object"; return -1; }
    std::vector<lm::SceneItem> scene;
    scene.reserve(items.size());
    for (const auto& raw : items) {
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
        scene.push_back(it);
    }
    auto r = impl_->renderer->present(state.impl().state, *output.impl().output, scene, lm::Color{bg_r, bg_g, bg_b, bg_a});
    if (!r) { error = to_rust(r.error().message); return -1; }
    return static_cast<int>(*r);
}
void SceneRendererShim::set_presentation(const PresentationShim& presentation) {
    if (impl_->renderer && presentation.impl().mgr) {
        impl_->renderer->set_presentation(&*presentation.impl().mgr);
    }
}
void SceneRendererShim::clear_presentation() {
    if (impl_->renderer) {
        impl_->renderer->set_presentation(nullptr);
    }
}
void SceneRendererShim::update_cursor() {
    if (impl_->renderer) impl_->renderer->update_cursor();
}
rust::Vec<std::uint8_t> SceneRendererShim::read_back(SceneOutputShim& state, rust::String& error) {
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
void OutputLayoutShim::add(OutputShim& output, std::int32_t x, std::int32_t y) {
    lm::Output* out = output.impl().output;
    if (!out) return;
    impl_->layout.add(*out, x, y);
    impl_->watch(out);
}
void OutputLayoutShim::add_auto(OutputShim& output) {
    lm::Output* out = output.impl().output;
    if (!out) return;
    impl_->layout.add_auto(*out);
    impl_->watch(out);
}
Rect OutputLayoutShim::bounds() const {
    const auto b = impl_->layout.bounds();
    return Rect{b.x, b.y, b.width, b.height};
}
void OutputLayoutShim::remove(OutputShim& output) {
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

std::unique_ptr<CursorShim> cursor_new(SceneRendererShim& renderer, const OutputLayoutShim& layout,
                                      SeatShim& seat, rust::String& error) {
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
};
FrameShim::~FrameShim() = default;
FrameShim::FrameShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
FrameShim::Impl& FrameShim::impl() const { return *impl_; }

std::unique_ptr<FrameShim> frame_new(OutputShim& output, VulkanRendererShim& renderer, rust::String& error) {
    if (!output.impl().output || !renderer.impl().renderer) {
        error = "null output or renderer";
        return nullptr;
    }
    auto impl = std::make_unique<FrameShim::Impl>();
    impl->frame = std::make_unique<lm::Frame>(*output.impl().output, *renderer.impl().renderer);
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
    std::vector<lm::SceneItem> scene;
    scene.reserve(items.size());
    for (const auto& raw : items) {
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
        scene.push_back(it);
    }
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

void foreign_toplevel_track(ForeignToplevelManagerShim& ftm, XdgShellShim& shell) {
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

bool session_lock_manager_locked(SessionLockManagerShim& slm) {
    return slm.impl().mgr ? slm.impl().mgr->session_locked() : false;
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
    std::optional<lm::InputMethodManager> mgr;
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
        return std::make_unique<InputMethodManagerShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
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

struct PopupTreeShim::Impl {
    lm::PopupTree tree;
};
PopupTreeShim::~PopupTreeShim() = default;
PopupTreeShim::PopupTreeShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PopupTreeShim::Impl& PopupTreeShim::impl() const { return *impl_; }

std::unique_ptr<PopupTreeShim> popup_tree_new() {
    return std::make_unique<PopupTreeShim>(std::make_unique<PopupTreeShim::Impl>());
}
bool PopupTreeShim::contains(std::uint64_t surface_id) const {
    return impl_->tree.contains(id_from_u64(surface_id));
}
bool PopupTreeShim::dismiss_grabs_outside(std::uint64_t surface_id) {
    return impl_->tree.dismiss_grabs_outside(id_from_u64(surface_id));
}

} // namespace lum
