// luminaria_rs.cpp — the C++ half of the Rust binding, core module only.
//
// The binding is split the way luminaria itself is (ADR 0008): this TU imports
// `luminaria;` and nothing else, so a consumer that leaves the `shell` and
// `desktop` cargo features off never compiles those shims and never links
// Vulkan, libdrm, libinput, libudev, gbm or libseat.
//
//   luminaria_rs.cpp         the core module: protocols, utilities, Display, Output
//   luminaria_rs_gpu.cpp     import luminaria.shell (backend + render + shell: the
//                            headless/nested/DRM backends, Vulkan, Frame, Cursor,
//                            SceneRenderer, direct scanout, layer/popup helpers)
//   luminaria_rs_desktop.cpp import luminaria.desktop  (privileged desktop protocols)
//   luminaria_rs_text.cpp    import luminaria.text     (fcft + pixman)
//
// These are ordinary translation units that import a module, not module units,
// so they have no `module;` line: includes first, then the import, exactly like
// examples/minimal.cpp.
//
// What they are: one thin shim per library type, declared in luminaria_rs*.h.
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
#include "luminaria/src/ffi.rs.h"

import luminaria;

#include "luminaria_rs_internal.h"

namespace lum {

// ============================================================================
// Core & Util
// ============================================================================

std::deque<HubRecord>& hub() {
    thread_local std::deque<HubRecord> queue;
    return queue;
}

void hub_push_output(std::unique_ptr<OutputShim> output) {
    HubRecord record{HubRecord::Type::output_added, event(EventKind::OutputAdded, output->id()), {}, nullptr};
    record.output = std::move(output);
    hub().push_back(std::move(record));
}

struct DisplayShim::Impl {
    std::optional<lm::Display> display;
    // An added output between next_event() and take_output().
    std::unique_ptr<OutputShim> taken;

    // Whatever the display's own teardown queued is nobody's any more.
    ~Impl() {
        display.reset();
        hub().clear();
    }
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

std::uint32_t DisplayShim::next_event(EventRaw& out, InputEventRaw& input) const {
    std::deque<HubRecord>& queue = hub();
    if (queue.empty()) return 0;
    HubRecord record = std::move(queue.front());
    queue.pop_front();
    out = std::move(record.event);
    input = std::move(record.input);
    impl_->taken = std::move(record.output);
    return static_cast<std::uint32_t>(record.type);
}

std::unique_ptr<OutputShim> DisplayShim::take_output() const { return std::move(impl_->taken); }

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


struct OutputShim::Impl {
    lm::Output* output = nullptr;
    // Identity that outlives the output, so Rust can match a removal to the
    // Output it holds. Never reused.
    std::uint64_t id = 0;
    Events events;
    Conn<lm::OutputDestroy> on_destroy;
    Conn<lm::FrameEvent> on_frame;
    Conn<lm::OutputModeChange> on_mode;
    Conn<lm::PresentEvent> on_present;
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


bool OutputShim::alive() const { return impl_->output != nullptr; }

std::uint64_t OutputShim::id() const { return impl_->id; }

std::unique_ptr<OutputShim> make_output_shim(lm::Output* output) {
    static std::atomic<std::uint64_t> next_id{1};
    if (!output) return nullptr;
    auto impl = std::make_unique<OutputShim::Impl>();
    impl->output = output;
    impl->id = next_id.fetch_add(1, std::memory_order_relaxed);
    impl->on_mode = output->mode_changed.connect([impl = impl.get()](lm::OutputModeChange&) {
        impl->events.push(event(EventKind::OutputModeChanged, impl->id));
    });
    impl->on_destroy = output->destroy.connect([impl = impl.get()](lm::OutputDestroy&) {
        impl->output = nullptr;
    });
    impl->on_frame = output->frame.connect([impl = impl.get()](lm::FrameEvent&) {
        impl->events.push(event(EventKind::OutputFrame, impl->id));
    });
    impl->on_present = output->present.connect([impl = impl.get()](lm::PresentEvent& e) {
        // `id` names the output, as every output event does; the vblank
        // counter rides in x (low half) and y (high half).
        EventRaw ev = event(EventKind::OutputPresent, impl->id, e.tv_sec);
        ev.serial = e.tv_nsec;
        ev.x = static_cast<std::int32_t>(static_cast<std::uint32_t>(e.seq));
        ev.y = static_cast<std::int32_t>(static_cast<std::uint32_t>(e.seq >> 32));
        ev.value = e.refresh_ns;
        ev.flag = e.vsync;
        ev.flag2 = e.hw_clock;
        impl->events.push(std::move(ev));
    });
    return std::make_unique<OutputShim>(std::move(impl));
}

// ============================================================================
// Protocols
// ============================================================================

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
void surface_notify_rendered(std::uint64_t surface_id, int fence_fd) {
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    if (s) s->notify_rendered(fence_fd);
}
bool surface_accepts_input(std::uint64_t surface_id, double sx, double sy) {
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    return s ? s->accepts_input(sx, sy) : false;
}
rust::Vec<SurfaceAtRaw> surface_tree(std::uint64_t surface_id) {
    rust::Vec<SurfaceAtRaw> out;
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    if (s == nullptr) return out;
    std::vector<lm::SurfaceAt> tree;
    s->surface_tree(tree);
    for (const lm::SurfaceAt& at : tree) {
        out.push_back(SurfaceAtRaw{u64_from_id(at.surface->id()), at.x, at.y});
    }
    return out;
}
void surface_set_preferred_buffer_scale(std::uint64_t surface_id, std::int32_t scale) {
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    if (s) s->set_preferred_buffer_scale(scale);
}
void surface_set_preferred_buffer_transform(std::uint64_t surface_id, std::uint32_t transform) {
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    if (s) s->set_preferred_buffer_transform(static_cast<lm::Transform>(transform));
}

struct CompositorShim::Impl {
    struct SurfaceEntry {
        Conn<lm::SurfaceCommit> on_commit;
        Conn<lm::SurfaceDestroy> on_destroy;
        Conn<lm::SurfaceRendered> on_rendered;
    };
    std::optional<lm::Compositor> comp;
    std::unordered_map<std::uint64_t, std::unique_ptr<SurfaceEntry>> surfaces;
    Events events;
    Conn<lm::NewSurface> on_new_surface;
    Conn<lm::SurfaceInvalidated> on_invalidated;

    void watch(lm::Surface& surface) {
        const std::uint64_t id = u64_from_id(surface.id());
        auto entry = std::make_unique<SurfaceEntry>();
        entry->on_commit = surface.commit.connect([this, id](lm::SurfaceCommit&) {
            events.push(event(EventKind::SurfaceCommit, id));
        });
        entry->on_rendered = surface.rendered.connect([this, id](lm::SurfaceRendered&) {
            events.push(event(EventKind::SurfaceRendered, id));
        });
        entry->on_destroy = surface.destroy.connect([this, id](lm::SurfaceDestroy&) {
            events.push(event(EventKind::SurfaceDestroy, id));
            surfaces.erase(id);
        });
        events.push(event(EventKind::NewSurface, id));
        surfaces[id] = std::move(entry);
    }
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
        auto* raw = impl.get();
        raw->on_new_surface = raw->comp->new_surface().connect([raw](lm::NewSurface& e) {
            raw->watch(e.surface);
        });
        raw->on_invalidated = lm::surface_invalidated().connect([raw](lm::SurfaceInvalidated& e) {
            raw->events.push(event(EventKind::SurfaceInvalidated, u64_from_id(e.surface)));
        });
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
    // Listening to a request is what turns the library's own answer off, so
    // the three window-state requests are only listened to once Rust asks.
    bool forward_requests = false;
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
        entry->on_identity = t.identity_change.connect(plain(EventKind::ToplevelIdentityChange));
        entry->on_state = t.state_change.connect(plain(EventKind::ToplevelStateChange));
        if (forward_requests) {
            entry->on_minimize = t.request_minimize.connect(plain(EventKind::ToplevelRequestMinimize));
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
        }
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

void XdgShellShim::focus(const SeatShim& seat, std::uint64_t surface_id) const {
    lm::Seat* s = raw_ptr(seat);
    if (impl_->shell && s) impl_->shell->focus(*s, surface_id != 0 ? impl_->toplevel(surface_id) : nullptr);
}

void XdgShellShim::forward_window_requests() const { impl_->forward_requests = true; }


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
void SeatShim::release_keys() const {
    if (impl_->seat) impl_->seat->release_keys();
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
void SeatShim::pointer_axis_event(double dx, double dy, std::int32_t dx_steps, std::int32_t dy_steps,
                                  bool stop_horizontal, bool stop_vertical) const {
    if (impl_->seat) {
        impl_->seat->pointer_axis(
            lm::PointerAxisEvent{dx, dy, dx_steps, dy_steps, stop_horizontal, stop_vertical});
    }
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

std::unique_ptr<OutputGlobalShim> output_global_for(const DisplayShim& display, const OutputShim& output,
                                                   const OutputLayoutShim& layout, rust::Str name,
                                                   rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        lm::Output* out = output.impl().output;
        if (out == nullptr) { error = "output destroyed"; return nullptr; }
        auto r = lm::OutputGlobal::create(*display.impl().display, *out, raw_ref(layout), std::string(name));
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

std::int32_t OutputGlobalShim::logical_x() const { return impl_->global ? impl_->global->logical_x() : 0; }
std::int32_t OutputGlobalShim::logical_y() const { return impl_->global ? impl_->global->logical_y() : 0; }
std::int32_t OutputGlobalShim::logical_width() const { return impl_->global ? impl_->global->logical_width() : 0; }
std::int32_t OutputGlobalShim::logical_height() const { return impl_->global ? impl_->global->logical_height() : 0; }
std::int32_t OutputGlobalShim::width() const { return impl_->global ? impl_->global->width() : 0; }
std::int32_t OutputGlobalShim::height() const { return impl_->global ? impl_->global->height() : 0; }
std::int32_t OutputGlobalShim::scale() const { return impl_->global ? impl_->global->scale() : 1; }
std::uint32_t OutputGlobalShim::transform() const { return impl_->global ? static_cast<std::uint32_t>(impl_->global->transform()) : 0; }
std::int32_t OutputGlobalShim::refresh_mhz() const { return impl_->global ? impl_->global->refresh_mhz() : 0; }
rust::String OutputGlobalShim::name() const { return impl_->global ? to_rust(impl_->global->name()) : rust::String(""); }

bool LayerShellShim::on_output(std::uint64_t surface_id, const OutputGlobalShim& output) const {
    const lm::LayerSurface* ls = impl_->find(surface_id);
    return ls != nullptr && is_output(ls->output_resource(), output);
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
DEFINE_GLOBAL_SHIM_IMPL(SinglePixelBufferManager, SinglePixelBufferManager, single_pixel_buffer_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(TearingControlManager, TearingControlManager, tearing_control_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(ContentTypeManager, ContentTypeManager, content_type_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(FifoManager, FifoManager, fifo_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(CommitTimingManager, CommitTimingManager, commit_timing_manager_new)
DEFINE_GLOBAL_SHIM_IMPL(BackgroundEffectManager, BackgroundEffectManager, background_effect_manager_new)

#undef DEFINE_GLOBAL_SHIM_IMPL

// ---- fractional-scale ------------------------------------------------------
//
// The scale is per surface and per output: the compositor calls set_scale()
// when a window lands on an output and again when that output's scale changes.

struct FractionalScaleManagerShim::Impl {
    std::optional<lm::FractionalScaleManager> mgr;
};
FractionalScaleManagerShim::~FractionalScaleManagerShim() = default;
FractionalScaleManagerShim::FractionalScaleManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
FractionalScaleManagerShim::Impl& FractionalScaleManagerShim::impl() const { return *impl_; }
std::unique_ptr<FractionalScaleManagerShim> fractional_scale_manager_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::FractionalScaleManager::create(*display.impl().display);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<FractionalScaleManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        return std::make_unique<FractionalScaleManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}
void FractionalScaleManagerShim::set_scale(std::uint64_t surface_id, double scale) const {
    if (!impl_->mgr) return;
    lm::Surface* s = lm::surface_from_id(id_from_u64(surface_id));
    if (s) impl_->mgr->set_scale(*s, scale);
}

// ---- xdg-decoration --------------------------------------------------------

struct XdgDecorationManagerShim::Impl {
    std::optional<lm::XdgDecorationManager> mgr;
    Events events;
    Conn<lm::DecorationRequest> on_request;
};
XdgDecorationManagerShim::~XdgDecorationManagerShim() = default;
XdgDecorationManagerShim::XdgDecorationManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
XdgDecorationManagerShim::Impl& XdgDecorationManagerShim::impl() const { return *impl_; }

std::unique_ptr<XdgDecorationManagerShim> xdg_decoration_manager_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::XdgDecorationManager::create(*display.impl().display);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<XdgDecorationManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        auto* raw = impl.get();
        raw->on_request = raw->mgr->request().connect([raw](lm::DecorationRequest& e) {
            EventRaw ev = event(EventKind::DecorationRequest,
                                e.toplevel != nullptr ? surface_u64(&e.toplevel->surface()) : 0);
            ev.value = static_cast<std::uint32_t>(e.mode);
            ev.serial = e.preferred.has_value() ? static_cast<std::uint32_t>(*e.preferred) : 0;
            raw->events.push(std::move(ev));
        });
        return std::make_unique<XdgDecorationManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}

void XdgDecorationManagerShim::set_default_mode(std::uint32_t mode) const {
    if (impl_->mgr) impl_->mgr->set_default_mode(static_cast<lm::DecorationMode>(mode));
}
std::uint32_t XdgDecorationManagerShim::default_mode() const {
    return impl_->mgr ? static_cast<std::uint32_t>(impl_->mgr->default_mode()) : 1;
}

// ---- cursor-shape ----------------------------------------------------------

struct CursorShapeManagerShim::Impl {
    std::optional<lm::CursorShapeManager> mgr;
    Events events;
    Conn<lm::CursorShapeRequest> on_request;
};
CursorShapeManagerShim::~CursorShapeManagerShim() = default;
CursorShapeManagerShim::CursorShapeManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CursorShapeManagerShim::Impl& CursorShapeManagerShim::impl() const { return *impl_; }

std::unique_ptr<CursorShapeManagerShim> cursor_shape_manager_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::CursorShapeManager::create(*display.impl().display);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<CursorShapeManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        auto* raw = impl.get();
        raw->on_request = raw->mgr->request().connect([raw](lm::CursorShapeRequest& req) {
            EventRaw ev = event(EventKind::CursorShapeRequest);
            ev.serial = req.serial;
            ev.value = req.shape;
            ev.text = req.name ? to_rust(req.name) : rust::String("");
            raw->events.push(std::move(ev));
        });
        return std::make_unique<CursorShapeManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}

// ---- xdg-toplevel-icon -----------------------------------------------------

struct XdgToplevelIconManagerShim::Impl {
    std::optional<lm::XdgToplevelIconManager> mgr;
    Events events;
    std::unordered_map<std::uint64_t, std::string> icon_names;
    Conn<lm::ToplevelIconChange> on_changed;
};
XdgToplevelIconManagerShim::~XdgToplevelIconManagerShim() = default;
XdgToplevelIconManagerShim::XdgToplevelIconManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
XdgToplevelIconManagerShim::Impl& XdgToplevelIconManagerShim::impl() const { return *impl_; }

std::unique_ptr<XdgToplevelIconManagerShim> xdg_toplevel_icon_manager_new(const DisplayShim& display, rust::String& error) {
    try {
        if (!display.impl().display) { error = "display not initialized"; return nullptr; }
        auto r = lm::XdgToplevelIconManager::create(*display.impl().display);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<XdgToplevelIconManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        auto* raw = impl.get();
        raw->on_changed = raw->mgr->changed().connect([raw](lm::ToplevelIconChange& e) {
            const std::uint64_t id = surface_u64(&e.toplevel.surface());
            EventRaw ev = event(EventKind::ToplevelIconChange, id);
            const auto* icon = raw->mgr->icon_for(e.toplevel);
            std::string name = icon != nullptr ? icon->name : "";
            raw->icon_names[id] = name;
            ev.text = to_rust(name);
            raw->events.push(std::move(ev));
        });
        return std::make_unique<XdgToplevelIconManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}
rust::String XdgToplevelIconManagerShim::icon_name(std::uint64_t surface_id) const {
    auto it = impl_->icon_names.find(surface_id);
    return it != impl_->icon_names.end() ? to_rust(it->second) : "";
}

// ---- xdg-activation --------------------------------------------------------

struct XdgActivationShim::Impl {
    std::optional<lm::XdgActivation> mgr;
    Events events;
    Conn<lm::ActivationRequest> on_activate;
    Conn<lm::ActivationTokenRequest> on_token;
    bool grant_tokens = true;
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
        raw->on_token = raw->mgr->new_token().connect([raw](lm::ActivationTokenRequest& e) {
            e.granted = raw->grant_tokens;
            EventRaw ev = event(EventKind::ActivationTokenRequest, surface_u64(e.surface));
            ev.serial = e.serial;
            ev.flag = e.granted;
            ev.text = to_rust(e.app_id);
            raw->events.push(std::move(ev));
        });
        return std::make_unique<XdgActivationShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}
void XdgActivationShim::set_grant_tokens(bool grant) const { impl_->grant_tokens = grant; }

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

// ---- selections ------------------------------------------------------------

namespace {

// DataDeviceManager and PrimarySelectionManager have the same three hooks.
// A change of owner reaches Rust with what the new owner offers in `text`,
// one mime type per line (a mime type cannot hold a newline).
template <class Manager, EventKind Kind>
struct SelectionShimImpl {
    std::optional<Manager> mgr;
    Conn<lm::SelectionChange> on_change;

    void watch() {
        on_change = mgr->selection_changed().connect([](lm::SelectionChange& e) {
            std::string joined;
            for (const std::string& mime : e.mime_types) {
                if (!joined.empty()) joined += '\n';
                joined += mime;
            }
            EventRaw ev = event(Kind);
            ev.text = to_rust(joined);
            hub_push(std::move(ev));
        });
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

struct DataDeviceManagerShim::Impl : SelectionShimImpl<lm::DataDeviceManager, EventKind::SelectionChange> {};
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
rust::Vec<rust::String> DataDeviceManagerShim::selection_mime_types() const { return impl_->mime_types(); }
bool DataDeviceManagerShim::selection_receive(rust::Str mime, int fd) const { return impl_->receive(mime, fd); }

struct PrimarySelectionManagerShim::Impl : SelectionShimImpl<lm::PrimarySelectionManager, EventKind::PrimarySelectionChange> {};
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

struct OutputLayoutShim::Impl {
    // Follows its outputs by itself: one that dies leaves at once, before Rust
    // hears of it, which is what the Cursor reading it during dispatch needs.
    lm::OutputLayout layout;
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
}
void OutputLayoutShim::add_auto(OutputShim& output) const {
    lm::Output* out = output.impl().output;
    if (!out) return;
    impl_->layout.add_auto(*out);
}
Rect OutputLayoutShim::bounds() const {
    const auto b = impl_->layout.bounds();
    return Rect{b.x, b.y, b.width, b.height};
}
void OutputLayoutShim::remove(OutputShim& output) const {
    lm::Output* out = output.impl().output;
    if (!out) return;
    impl_->layout.remove(*out);
}
Rect OutputLayoutShim::primary_box() const {
    const auto b = impl_->layout.primary_box();
    return Rect{b.x, b.y, b.width, b.height};
}
Rect OutputLayoutShim::output_box(const OutputShim& output) const {
    if (!output.impl().output) return Rect{0, 0, 0, 0};
    const auto b = impl_->layout.box_of(*output.impl().output);
    return Rect{b.x, b.y, b.width, b.height};
}

// ---------------------------------------------------------------------------
// The few things the other shim TUs reach in here for. At the end of this
// file because they read Impl members, and Impl is per-shim below.
// ---------------------------------------------------------------------------

lm::Display* raw_ptr(const DisplayShim& s) { return s.impl().display ? &*s.impl().display : nullptr; }
lm::Output* raw_ptr(const OutputShim& s) { return s.impl().output; }
lm::OutputLayout& raw_ref(const OutputLayoutShim& s) { return s.impl().layout; }
lm::Seat* raw_ptr(const SeatShim& s) { return s.impl().seat ? &*s.impl().seat : nullptr; }
lm::XdgShell* raw_ptr(const XdgShellShim& s) { return s.impl().shell ? &*s.impl().shell : nullptr; }
lm::CursorShapeManager* raw_ptr(const CursorShapeManagerShim& s) { return s.impl().mgr ? &*s.impl().mgr : nullptr; }
lm::DataDeviceManager* raw_ptr(const DataDeviceManagerShim& s) { return s.impl().mgr ? &*s.impl().mgr : nullptr; }
lm::TextInputManager* raw_ptr(const TextInputManagerShim& s) { return s.impl().mgr ? &*s.impl().mgr : nullptr; }

lm::Popup* xdg_popup(const XdgShellShim& shell, std::uint64_t popup) {
    return shell.impl().popup(popup);
}
lm::LayerSurface* layer_surface(const LayerShellShim& shell, std::uint64_t surface_id) {
    return shell.impl().find(surface_id);
}

// Identity for an object that has no surface of its own to be named by (a
// lock, an idle notification, a text input). Never reused.
std::uint64_t next_object_id() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

// Whether `resource` (a client's wl_output) is `global`'s.
bool is_output(wl_resource* resource, const OutputGlobalShim& global) {
    const auto& g = global.impl().global;
    return resource != nullptr && g && g->resource_for(wl_resource_get_client(resource)) == resource;
}


} // namespace lum