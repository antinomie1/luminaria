// luminaria_rs_gpu.cpp — C++ shim for luminaria.gpu (Vulkan, DRM, libinput, session).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "luminaria/luminaria_rs.h"
#include "luminaria/luminaria_rs_gpu.h"
#include "luminaria/src/ffi.rs.h"
#include "luminaria/src/ffi_gpu.rs.h"

import luminaria.gpu;

#include "luminaria_rs_internal.h"

namespace lum {

// ============================================================================
// Bare-metal Backends (Session, DRM, Libinput)
// ============================================================================

struct SessionShim::Impl {
    // `session` is `owned`, or one an AutoBackendShim lends out.
    std::unique_ptr<lm::Session> owned;
    lm::Session* session = nullptr;
    std::deque<bool> activity;
    Conn<lm::SessionActive> on_activity;
};

SessionShim::~SessionShim() = default;
SessionShim::SessionShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SessionShim::Impl& SessionShim::impl() const { return *impl_; }

std::unique_ptr<SessionShim> session_new(const DisplayShim& display, rust::String& error) {
    try {
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        auto r = lm::Session::create(disp->event_loop());
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<SessionShim::Impl>();
        impl->owned = std::make_unique<lm::Session>(std::move(*r));
        impl->session = impl->owned.get();
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

namespace {

// A backend's outputs, arriving and leaving, queued for Rust.
struct OutputQueue {
    struct Event {
        std::unique_ptr<OutputShim> added; // null: a removal
        std::uint64_t removed = 0;
    };
    // Hotplug watch on each output the backend reported, keyed by the shim's id.
    struct Watch {
        const lm::Output* output;
        std::uint64_t id;
        Conn<lm::OutputDestroy> on_destroy;
    };

    std::deque<Event> events;
    std::deque<Watch> watches;
    Conn<lm::NewOutput> on_new_output;

    // Every output, the ones start() finds included, reaches Rust as an event:
    // C++ never calls Rust, so a signal can only leave something to be drained.
    void watch(lm::Backend& backend) {
        on_new_output = backend.new_output.connect([this](lm::NewOutput& e) {
            auto shim = make_output_shim(&e.output);
            const std::uint64_t id = shim->id();
            Watch& w = watches.emplace_back();
            w.output = &e.output;
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

    // The shim id `output` was reported under; 0 for one this queue never saw.
    std::uint64_t id_of(const lm::Output* output) const {
        for (const Watch& w : watches) {
            if (w.output == output) return w.id;
        }
        return 0;
    }

    std::unique_ptr<OutputShim> next(std::uint32_t& kind, std::uint64_t& removed) {
        kind = 0;
        if (events.empty()) return nullptr;
        Event e = std::move(events.front());
        events.pop_front();
        if (e.added) {
            kind = 1;
            return std::move(e.added);
        }
        kind = 2;
        removed = e.removed;
        return nullptr;
    }
};

} // namespace

struct DrmBackendShim::Impl {
    std::unique_ptr<lm::DrmBackend> backend;
    OutputQueue outputs;

    void watch() { outputs.watch(*backend); }
};

DrmBackendShim::~DrmBackendShim() = default;
DrmBackendShim::DrmBackendShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DrmBackendShim::Impl& DrmBackendShim::impl() const { return *impl_; }

std::unique_ptr<DrmBackendShim> drm_backend_new(const DisplayShim& display, rust::Str device, const SessionShim& session, rust::String& error) {
    try {
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        auto loop = disp->event_loop();
        lm::Session* sess = session.impl().session;
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
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        auto loop = disp->event_loop();
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
    return impl_->outputs.next(kind, removed);
}

struct LibinputBackendShim::Impl {
    std::unique_ptr<lm::LibinputBackend> backend;
    std::deque<InputEventRaw> events;
    Conn<lm::InputEvent> on_input;

    // A key's keysym and masks are the backend's reading as the key arrived,
    // carried on the event itself — Rust reads the queue only after dispatch()
    // returns, by which time later keys of the same batch have moved the
    // backend's state on.
    void watch() {
        on_input = backend->input().connect(
            [this](lm::InputEvent& e) { events.push_back(input_event_raw(e)); });
    }
};

LibinputBackendShim::~LibinputBackendShim() = default;
LibinputBackendShim::LibinputBackendShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LibinputBackendShim::Impl& LibinputBackendShim::impl() const { return *impl_; }

namespace {

std::unique_ptr<LibinputBackendShim> make_libinput(const DisplayShim& display, lm::Session* session,
                                                   bool path, rust::String& error) {
    try {
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        auto loop = disp->event_loop();
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
    return make_libinput(display, session.impl().session, false, error);
}

std::unique_ptr<LibinputBackendShim> libinput_backend_new_standalone(const DisplayShim& display, rust::String& error) {
    return make_libinput(display, nullptr, false, error);
}

std::unique_ptr<LibinputBackendShim> libinput_backend_new_path(const DisplayShim& display, const SessionShim& session, rust::String& error) {
    return make_libinput(display, session.impl().session, true, error);
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

bool LibinputBackendShim::next_input_event(InputEventRaw& out) {
    if (impl_->events.empty()) return false;
    out = std::move(impl_->events.front());
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
    return impl_->backend ? caps_bits(impl_->backend->capabilities()) : 0;
}

struct AutoBackendShim::Impl {
    std::optional<lm::AutoBackend> backend;
    // Lends out the backend's session, so it goes first.
    std::unique_ptr<SessionShim> session;
    OutputQueue outputs;
    std::deque<InputEventRaw> input_events;
    bool close_requested = false;
    Conn<lm::InputEvent> on_input;
    Conn<lm::HostCloseRequest> on_close;
};

AutoBackendShim::~AutoBackendShim() = default;
AutoBackendShim::AutoBackendShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AutoBackendShim::Impl& AutoBackendShim::impl() const { return *impl_; }

std::unique_ptr<AutoBackendShim> auto_backend_new(const DisplayShim& display, std::uint32_t kind, rust::Str drm_device,
                                                  std::int32_t width, std::int32_t height, rust::Str title,
                                                  rust::String& error) {
    try {
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        constexpr lm::BackendKind kinds[] = {lm::BackendKind::automatic, lm::BackendKind::nested,
                                             lm::BackendKind::drm, lm::BackendKind::headless};
        if (kind >= std::size(kinds)) { error = "unknown backend kind"; return nullptr; }
        lm::BackendOptions options{.kind = kinds[kind],
                                   .drm_device = std::string(drm_device),
                                   .width = width,
                                   .height = height,
                                   .title = std::string(title)};
        auto r = lm::AutoBackend::create(disp->event_loop(), options);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<AutoBackendShim::Impl>();
        impl->backend.emplace(std::move(*r));
        lm::AutoBackend& backend = *impl->backend;
        auto* raw = impl.get();
        raw->outputs.watch(backend.backend());
        if (lm::Session* s = backend.session()) {
            auto session = std::make_unique<SessionShim::Impl>();
            session->session = s;
            session->on_activity = s->activity().connect(
                [q = session.get()](lm::SessionActive& e) { q->activity.push_back(e.active); });
            raw->session = std::make_unique<SessionShim>(std::move(session));
        }
        if (lm::InputSource* source = backend.input()) {
            // Read as the event arrives, as LibinputBackendShim does; absolute
            // motion names its output by the id Rust was handed.
            raw->on_input = source->input().connect([raw](lm::InputEvent& e) {
                InputEventRaw ev = input_event_raw(e);
                if (const auto* motion = std::get_if<lm::PointerMotionAbsEvent>(&e)) {
                    ev.output = raw->outputs.id_of(motion->output);
                }
                raw->input_events.push_back(std::move(ev));
            });
        }
        if (lm::WaylandBackend* nested = backend.nested()) {
            raw->on_close = nested->close_requested.connect(
                [raw](lm::HostCloseRequest&) { raw->close_requested = true; });
        }
        return std::make_unique<AutoBackendShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::uint32_t AutoBackendShim::kind() const {
    switch (impl_->backend->kind()) {
    case lm::BackendKind::nested: return 1;
    case lm::BackendKind::drm: return 2;
    default: return 3;
    }
}

bool AutoBackendShim::start(rust::String& error) {
    auto st = impl_->backend->start();
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

std::unique_ptr<OutputShim> AutoBackendShim::next_event(std::uint32_t& kind, std::uint64_t& removed) {
    return impl_->outputs.next(kind, removed);
}

bool AutoBackendShim::next_input_event(InputEventRaw& out) {
    if (impl_->input_events.empty()) return false;
    out = std::move(impl_->input_events.front());
    impl_->input_events.pop_front();
    return true;
}

rust::String AutoBackendShim::keymap() const {
    const lm::InputSource* source = impl_->backend->input();
    return source ? to_rust(source->keymap()) : rust::String{};
}

std::uint32_t AutoBackendShim::capabilities() const {
    const lm::InputSource* source = impl_->backend->input();
    return source ? caps_bits(source->capabilities()) : 0;
}

bool AutoBackendShim::take_close_request() { return std::exchange(impl_->close_requested, false); }

bool AutoBackendShim::has_session() const { return impl_->session != nullptr; }

const SessionShim& AutoBackendShim::session() const { return *impl_->session; }

rust::String AutoBackendShim::session_error() const { return to_rust(std::string(impl_->backend->session_error())); }

bool AutoBackendShim::drm_device(DrmDeviceId& out) const {
    const auto& id = impl_->backend->vulkan_options().drm_device;
    if (!id) return false;
    out = DrmDeviceId{id->major, id->minor};
    return true;
}

// ============================================================================
// GPU Protocols & Rendering
// ============================================================================

struct DrmSyncobjManagerShim::Impl { std::optional<lm::DrmSyncobjManager> mgr; };
DrmSyncobjManagerShim::~DrmSyncobjManagerShim() = default;
DrmSyncobjManagerShim::DrmSyncobjManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DrmSyncobjManagerShim::Impl& DrmSyncobjManagerShim::impl() const { return *impl_; }
std::unique_ptr<DrmSyncobjManagerShim> drm_syncobj_manager_new(const DisplayShim& display, rust::String& error) {
    try {
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        auto r = lm::DrmSyncobjManager::create(*disp);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<DrmSyncobjManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        return std::make_unique<DrmSyncobjManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}

struct ScreencopyManagerShim::Impl { std::optional<lm::ScreencopyManager> mgr; };
ScreencopyManagerShim::~ScreencopyManagerShim() = default;
ScreencopyManagerShim::ScreencopyManagerShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ScreencopyManagerShim::Impl& ScreencopyManagerShim::impl() const { return *impl_; }
std::unique_ptr<ScreencopyManagerShim> screencopy_manager_new(const DisplayShim& display, rust::String& error) {
    try {
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        auto r = lm::ScreencopyManager::create(*disp);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<ScreencopyManagerShim::Impl>();
        impl->mgr.emplace(std::move(*r));
        return std::make_unique<ScreencopyManagerShim>(std::move(impl));
    } catch (const std::exception& e) { error = e.what(); return nullptr; }
}

struct VulkanRendererShim::Impl {
    std::unique_ptr<lm::VulkanRenderer> renderer;
};
VulkanRendererShim::~VulkanRendererShim() = default;
VulkanRendererShim::VulkanRendererShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
VulkanRendererShim::Impl& VulkanRendererShim::impl() const { return *impl_; }

lm::VulkanRenderer* raw_ptr(const VulkanRendererShim& s) {
    return s.impl().renderer.get();
}

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
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        auto r = lm::LinuxDmabuf::create(*disp, renderer.impl().renderer.get());
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
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        auto r = lm::LinuxDmabuf::create(*disp, nullptr);
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
struct SceneRendererShim::Impl : PresentationObserver {
    std::unique_ptr<lm::SceneRenderer> renderer;
    PresentationShim::Impl* presentation = nullptr;
    std::unordered_map<std::uint64_t, StoredImage> images;

    void on_presentation_destroyed() override {
        presentation = nullptr;
        if (renderer) renderer->set_presentation(nullptr);
    }

    void link(PresentationShim::Impl* to) {
        if (presentation != nullptr) {
            std::erase(presentation->observers, static_cast<PresentationObserver*>(this));
        }
        presentation = to;
        if (to != nullptr) to->observers.push_back(this);
        if (renderer) renderer->set_presentation(to != nullptr && to->mgr ? &*to->mgr : nullptr);
    }

    ~Impl() override { link(nullptr); }
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

std::unique_ptr<LinuxDmabufShim> linux_dmabuf_new_from_scene_renderer(const DisplayShim& display, const SceneRendererShim& renderer, rust::String& error) {
    try {
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        lm::VulkanRenderer* gpu = renderer.impl().renderer ? renderer.impl().renderer->gpu() : nullptr;
        auto r = lm::LinuxDmabuf::create(*disp, gpu);
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<LinuxDmabufShim::Impl>();
        impl->dmabuf.emplace(std::move(*r));
        return std::make_unique<LinuxDmabufShim>(std::move(impl));
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
    lm::Output* out = raw_ptr(output);
    if (!impl_->renderer || !out) { error = "null object"; return false; }
    auto st = impl_->renderer->reset(state.impl().state, *out, drm_format);
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}
int SceneRendererShim::present(SceneOutputShim& state, OutputShim& output, rust::Slice<const SceneItemRaw> items,
                             float bg_r, float bg_g, float bg_b, float bg_a, rust::String& error) const {
    lm::Output* out = raw_ptr(output);
    if (!impl_->renderer || !out) { error = "null object"; return -1; }
    const std::vector<lm::SceneItem> scene = scene_items(items, &impl_->images);
    auto r = impl_->renderer->present(state.impl().state, *out, scene, lm::Color{bg_r, bg_g, bg_b, bg_a});
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

bool SceneRendererShim::set_image(std::uint64_t serial, std::int32_t width, std::int32_t height,
                                rust::Slice<const std::uint8_t> rgba, rust::String& error) const {
    if (width <= 0 || height <= 0) {
        error = "image dimensions must be positive";
        return false;
    }
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::size_t bytes = count * 4;
    if (rgba.size() < bytes) {
        error = "image buffer too small for dimensions";
        return false;
    }
    StoredImage img;
    img.width = width;
    img.height = height;
    img.pixels.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        img.pixels[i] = lm::Pixel{rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]};
    }
    impl_->images[serial] = std::move(img);
    return true;
}

void SceneRendererShim::remove_image(std::uint64_t serial) const {
    impl_->images.erase(serial);
}

void SceneRendererShim::clear_images() const {
    impl_->images.clear();
}

bool SceneRendererShim::has_image(std::uint64_t serial) const {
    return impl_->images.find(serial) != impl_->images.end();
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
        lm::Seat* seat_ptr = raw_ptr(seat);
        if (!renderer.impl().renderer || !seat_ptr) { error = "null renderer or seat"; return nullptr; }
        auto impl = std::make_unique<CursorShim::Impl>();
        impl->cursor = std::make_unique<lm::Cursor>(
            *renderer.impl().renderer, raw_ref(layout), *seat_ptr,
            nullptr, nullptr
        );
        return std::make_unique<CursorShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::unique_ptr<CursorShim> cursor_new_with_shapes(const SceneRendererShim& renderer, const OutputLayoutShim& layout,
                                                  const SeatShim& seat, const CursorShapeManagerShim& shapes,
                                                  rust::String& error) {
    try {
        lm::Seat* seat_ptr = raw_ptr(seat);
        if (!renderer.impl().renderer || !seat_ptr) { error = "null renderer or seat"; return nullptr; }
        auto impl = std::make_unique<CursorShim::Impl>();
        lm::CursorShapeManager* shapes_mgr = raw_ptr(shapes);
        impl->cursor = std::make_unique<lm::Cursor>(
            *renderer.impl().renderer, raw_ref(layout), *seat_ptr,
            shapes_mgr, nullptr
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
    lm::Output* out = raw_ptr(output);
    if (!out || !renderer.impl().renderer) {
        error = "null output or renderer";
        return nullptr;
    }
    auto impl = std::make_unique<FrameShim::Impl>();
    impl->frame = std::make_unique<lm::Frame>(*out, *renderer.impl().renderer);
    impl->output_gone = out->destroy.connect([raw = impl.get()](lm::OutputDestroy&) {
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

struct DirectScanoutShim::Impl {
    std::unique_ptr<lm::DirectScanout> ds;
};
DirectScanoutShim::~DirectScanoutShim() = default;
DirectScanoutShim::DirectScanoutShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DirectScanoutShim::Impl& DirectScanoutShim::impl() const { return *impl_; }

std::unique_ptr<DirectScanoutShim> direct_scanout_new(OutputShim& output) {
    lm::Output* out = raw_ptr(output);
    if (!out) return nullptr;
    auto impl = std::make_unique<DirectScanoutShim::Impl>();
    impl->ds = std::make_unique<lm::DirectScanout>(*out);
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

} // namespace lum
