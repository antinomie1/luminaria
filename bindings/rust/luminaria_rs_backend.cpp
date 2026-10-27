// luminaria_rs_backend.cpp — C++ shim for luminaria.backend.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "luminaria/luminaria_rs.h"
#include "luminaria/luminaria_rs_backend.h"
#include "luminaria/src/ffi.rs.h"
#include "luminaria/src/ffi_backend.rs.h"

import luminaria.backend;

#include "luminaria_rs_internal.h"

namespace lum {

namespace {
void forward_activity(lm::SessionActive& e) {
    hub_push(event(e.active ? EventKind::SessionActive : EventKind::SessionInactive));
}
} // namespace

struct SessionShim::Impl {
    std::unique_ptr<lm::Session> owned;
    lm::Session* session = nullptr;
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
        impl->on_activity = impl->session->activity().connect(forward_activity);
        return std::make_unique<SessionShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::unique_ptr<SessionShim> session_shim_from_raw(luminaria::Session* s) {
    if (!s) return nullptr;
    auto impl = std::make_unique<SessionShim::Impl>();
    impl->session = s;
    impl->on_activity = s->activity().connect(forward_activity);
    return std::make_unique<SessionShim>(std::move(impl));
}

bool SessionShim::switch_to(int vt, rust::String& error) const {
    if (!impl_->session) { error = "session not initialized"; return false; }
    auto st = impl_->session->switch_to(vt);
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

bool SessionShim::switch_vt_for(std::uint32_t keysym) const {
    return impl_->session ? impl_->session->switch_vt_for(keysym) : false;
}

bool SessionShim::active() const {
    return impl_->session ? impl_->session->active() : false;
}

rust::String SessionShim::seat_name() const {
    return "seat0";
}

lm::Session* raw_ptr(const SessionShim& s) {
    return s.impl().session;
}

namespace {

// A backend's outputs, arriving and leaving, forwarded to Rust.
struct OutputQueue {
    struct Watch {
        const lm::Output* output;
        std::uint64_t id;
        Conn<lm::OutputDestroy> on_destroy;
    };

    std::deque<Watch> watches;
    Conn<lm::NewOutput> on_new_output;

    void watch(lm::Backend& backend) {
        on_new_output = backend.new_output.connect([this](lm::NewOutput& e) {
            auto shim = make_output_shim(&e.output);
            const std::uint64_t id = shim->id();
            Watch& w = watches.emplace_back();
            w.output = &e.output;
            w.id = id;
            w.on_destroy = e.output.destroy.connect([this, id](lm::OutputDestroy&) {
                hub_push(event(EventKind::OutputRemoved, id));
                std::erase_if(watches, [id](const Watch& x) { return x.id == id; });
            });
            hub_push_output(std::move(shim));
        });
    }

    std::uint64_t id_of(const lm::Output* output) const {
        for (const Watch& w : watches) {
            if (w.output == output) return w.id;
        }
        return 0;
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

struct LibinputBackendShim::Impl {
    std::unique_ptr<lm::LibinputBackend> backend;
    Conn<lm::InputEvent> on_input;

    void watch() {
        on_input = backend->input().connect([](lm::InputEvent& e) { hub_push_input(input_event_raw(e)); });
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

rust::String LibinputBackendShim::keymap() const {
    return impl_->backend ? to_rust(impl_->backend->keymap()) : rust::String{};
}

bool LibinputBackendShim::set_keymap(rust::Str xkb_text) {
    return impl_->backend && impl_->backend->set_keymap(std::string(xkb_text));
}

void LibinputBackendShim::follow_seat(const SeatShim& seat, bool touch) const {
    if (impl_->backend && raw_ptr(seat) != nullptr) raw_ptr(seat)->follow(*impl_->backend, touch);
}

std::uint32_t LibinputBackendShim::capabilities() const {
    return impl_->backend ? caps_bits(impl_->backend->capabilities()) : 0;
}

struct HeadlessBackendShim::Impl {
    std::unique_ptr<lm::HeadlessBackend> backend;
};

HeadlessBackendShim::~HeadlessBackendShim() = default;
HeadlessBackendShim::HeadlessBackendShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
HeadlessBackendShim::Impl& HeadlessBackendShim::impl() const { return *impl_; }

std::unique_ptr<HeadlessBackendShim> headless_backend_new(const DisplayShim& display, unsigned int interval_ms, rust::String& error) {
    try {
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        auto backend = std::make_unique<lm::HeadlessBackend>(disp->event_loop(), interval_ms);
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
    std::vector<std::pair<const lm::Output*, std::uint64_t>> output_ids;
    Conn<lm::InputEvent> on_input;
    Conn<lm::HostCloseRequest> on_close;
};

WaylandBackendShim::~WaylandBackendShim() = default;
WaylandBackendShim::WaylandBackendShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
WaylandBackendShim::Impl& WaylandBackendShim::impl() const { return *impl_; }

std::unique_ptr<WaylandBackendShim> wayland_backend_new(const DisplayShim& display, rust::String& error) {
    try {
        lm::Display* disp = raw_ptr(display);
        if (!disp) { error = "display not initialized"; return nullptr; }
        auto r = lm::WaylandBackend::create(disp->event_loop());
        if (!r) { error = to_rust(r.error().message); return nullptr; }
        auto impl = std::make_unique<WaylandBackendShim::Impl>();
        impl->backend = std::make_unique<lm::WaylandBackend>(std::move(*r));
        auto* raw_impl = impl.get();
        raw_impl->on_input = raw_impl->backend->input().connect([raw_impl](lm::InputEvent& e) {
            InputEventRaw raw = input_event_raw(e);
            if (const auto* motion = std::get_if<lm::PointerMotionAbsEvent>(&e)) {
                for (const auto& [output, id] : raw_impl->output_ids) {
                    if (output == motion->output) raw.output = id;
                }
            }
            hub_push_input(std::move(raw));
        });
        raw_impl->on_close = raw_impl->backend->close_requested.connect(
            [](lm::HostCloseRequest&) { hub_push(event(EventKind::HostCloseRequest)); });
        return std::make_unique<WaylandBackendShim>(std::move(impl));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::unique_ptr<OutputShim> WaylandBackendShim::add_output(std::int32_t width, std::int32_t height) {
    if (!impl_->backend) return nullptr;
    lm::Output& out = impl_->backend->add_output(width, height);
    auto shim = make_output_shim(&out);
    impl_->output_ids.emplace_back(&out, shim->id());
    return shim;
}

bool WaylandBackendShim::start(rust::String& error) {
    if (!impl_->backend) { error = "backend not initialized"; return false; }
    auto st = impl_->backend->start();
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

rust::String WaylandBackendShim::keymap() const {
    return impl_->backend ? to_rust(impl_->backend->keymap()) : rust::String{};
}

std::uint32_t WaylandBackendShim::capabilities() const {
    return impl_->backend ? caps_bits(impl_->backend->capabilities()) : 0;
}

void WaylandBackendShim::follow_seat(const SeatShim& seat, bool touch) const {
    if (impl_->backend && raw_ptr(seat) != nullptr) raw_ptr(seat)->follow(*impl_->backend, touch);
}

} // namespace lum
