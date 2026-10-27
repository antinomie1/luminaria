// luminaria_rs_shell.cpp — C++ shim for luminaria.shell.
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
#include "luminaria/luminaria_rs_backend.h"
#include "luminaria/luminaria_rs_render.h"
#include "luminaria/luminaria_rs_shell.h"
#include "luminaria/src/ffi.rs.h"
#include "luminaria/src/ffi_backend.rs.h"
#include "luminaria/src/ffi_render.rs.h"
#include "luminaria/src/ffi_shell.rs.h"

import luminaria.shell;

#include "luminaria_rs_internal.h"

namespace lum {

namespace {

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
    for (const char* ext : h.device_extensions) out.device_extensions.push_back(to_rust(ext));
    return out;
}

inline lm::SceneItem scene_item(const SceneItemRaw& raw, const std::unordered_map<std::uint64_t, StoredImage>* images = nullptr) {
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
    if (it.kind == lm::SceneItem::Kind::image && images != nullptr) {
        auto it_img = images->find(it.serial);
        if (it_img != images->end()) {
            it.pixels = std::span<const lm::Pixel>{it_img->second.pixels.data(), it_img->second.pixels.size()};
        }
    }
    return it;
}

inline std::vector<lm::SceneItem> scene_items(rust::Slice<const SceneItemRaw> items, const std::unordered_map<std::uint64_t, StoredImage>* images = nullptr) {
    std::vector<lm::SceneItem> scene;
    scene.reserve(items.size());
    for (const auto& raw : items) scene.push_back(scene_item(raw, images));
    return scene;
}

inline SceneItemRaw scene_item_raw(const lm::SceneItem& it) {
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

// ============================================================================
// AutoBackend
// ============================================================================

struct AutoBackendShim::Impl {
    std::optional<lm::AutoBackend> backend;
    std::unique_ptr<SessionShim> session;
    OutputQueue outputs;
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
            raw->session = session_shim_from_raw(s);
        }
        if (lm::InputSource* source = backend.input()) {
            raw->on_input = source->input().connect([raw](lm::InputEvent& e) {
                InputEventRaw ev = input_event_raw(e);
                if (const auto* motion = std::get_if<lm::PointerMotionAbsEvent>(&e)) {
                    ev.output = raw->outputs.id_of(motion->output);
                }
                hub_push_input(std::move(ev));
            });
        }
        if (lm::WaylandBackend* nested = backend.nested()) {
            raw->on_close = nested->close_requested.connect(
                [](lm::HostCloseRequest&) { hub_push(event(EventKind::HostCloseRequest)); });
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

rust::String AutoBackendShim::keymap() const {
    const lm::InputSource* source = impl_->backend->input();
    return source ? to_rust(source->keymap()) : rust::String{};
}

std::uint32_t AutoBackendShim::capabilities() const {
    const lm::InputSource* source = impl_->backend->input();
    return source ? caps_bits(source->capabilities()) : 0;
}

void AutoBackendShim::follow_seat(const SeatShim& seat, bool touch) const {
    lm::InputSource* source = impl_->backend->input();
    if (source != nullptr && raw_ptr(seat) != nullptr) raw_ptr(seat)->follow(*source, touch);
}

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
// SceneRenderer
// ============================================================================

struct SceneRendererShim::Impl : PresentationObserver {
    std::unique_ptr<lm::SceneRenderer> renderer;
    PresentationShim::Impl* presentation = nullptr;
    std::unordered_map<std::uint64_t, StoredImage> images;

    struct State {
        lm::SceneOutput state;
        Conn<lm::OutputDestroy> on_destroy;
    };
    std::unordered_map<std::uint64_t, std::unique_ptr<State>> states;

    lm::SceneOutput& state_for(const OutputShim& output) {
        const std::uint64_t id = output.id();
        auto& slot = states[id];
        if (!slot) {
            slot = std::make_unique<State>();
            slot->on_destroy = raw_ptr(output)->destroy.connect(
                [this, id](lm::OutputDestroy&) { states.erase(id); });
        }
        return slot->state;
    }
    lm::SceneOutput* find_state(const OutputShim& output) const {
        const auto it = states.find(output.id());
        return it == states.end() ? nullptr : &it->second->state;
    }

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
    lm::VulkanRenderer* gpu = renderer.impl().renderer ? renderer.impl().renderer->gpu() : nullptr;
    return linux_dmabuf_from_raw(raw_ptr(display), gpu, error);
}

bool SceneRendererShim::gpu_enabled() const {
    return impl_->renderer ? impl_->renderer->gpu_enabled() : false;
}

rust::String SceneRendererShim::gpu_error() const {
    return impl_->renderer ? to_rust(std::string(impl_->renderer->gpu_error())) : "not initialized";
}

bool SceneRendererShim::reset(OutputShim& output, std::uint32_t drm_format, rust::String& error) const {
    lm::Output* out = raw_ptr(output);
    if (!impl_->renderer || !out) { error = "output destroyed"; return false; }
    auto st = impl_->renderer->reset(impl_->state_for(output), *out, drm_format);
    if (!st) { error = to_rust(st.error().message); return false; }
    return true;
}

int SceneRendererShim::present(OutputShim& output, rust::Slice<const SceneItemRaw> items,
                               float bg_r, float bg_g, float bg_b, float bg_a, rust::String& error) const {
    lm::Output* out = raw_ptr(output);
    if (!impl_->renderer || !out) { error = "output destroyed"; return -1; }
    const std::vector<lm::SceneItem> scene = scene_items(items, &impl_->images);
    auto r = impl_->renderer->present(impl_->state_for(output), *out, scene, lm::Color{bg_r, bg_g, bg_b, bg_a});
    if (!r) { error = to_rust(r.error().message); return -1; }
    return static_cast<int>(*r);
}

int SceneRendererShim::present_in(const OutputLayoutShim& layout, OutputShim& output,
                                  rust::Slice<const SceneItemRaw> items, float bg_r, float bg_g, float bg_b,
                                  float bg_a, rust::String& error) const {
    lm::Output* out = raw_ptr(output);
    if (!impl_->renderer || !out) { error = "output destroyed"; return -1; }
    const std::vector<lm::SceneItem> scene = scene_items(items, &impl_->images);
    auto r = impl_->renderer->present(impl_->state_for(output), *out, scene, lm::Color{bg_r, bg_g, bg_b, bg_a},
                                      raw_ref(layout));
    if (!r) { error = to_rust(r.error().message); return -1; }
    return static_cast<int>(*r);
}

bool SceneRendererShim::gpu_output(const OutputShim& output) const {
    const lm::SceneOutput* state = impl_->find_state(output);
    return state != nullptr && state->gpu_output();
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

rust::Vec<std::uint8_t> SceneRendererShim::read_back(const OutputShim& output, rust::String& error) const {
    rust::Vec<std::uint8_t> out;
    lm::SceneOutput* state = impl_->find_state(output);
    if (!impl_->renderer || state == nullptr) { error = "nothing presented on this output"; return out; }
    auto r = impl_->renderer->read_back(*state);
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

// ============================================================================
// Cursor & CursorTheme
// ============================================================================

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

bool CursorShim::move_to(double x, double y) {
    return impl_->cursor ? impl_->cursor->move_to(x, y) : false;
}
bool CursorShim::move_by(double dx, double dy) {
    return impl_->cursor ? impl_->cursor->move_by(dx, dy) : false;
}
bool CursorShim::move_to_within(double x, double y, Rect bounds) {
    return impl_->cursor ? impl_->cursor->move_to(x, y, lm::Box{bounds.x, bounds.y, bounds.width, bounds.height}) : false;
}
bool CursorShim::move_by_within(double dx, double dy, Rect bounds) {
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

// ============================================================================
// Frame
// ============================================================================

struct FrameShim::Impl {
    std::unique_ptr<lm::Frame> frame;
    Conn<lm::OutputDestroy> output_gone;
};
FrameShim::~FrameShim() = default;
FrameShim::FrameShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
FrameShim::Impl& FrameShim::impl() const { return *impl_; }

std::unique_ptr<FrameShim> frame_new(OutputShim& output, const VulkanRendererShim& renderer, rust::String& error) {
    lm::Output* out = raw_ptr(output);
    lm::VulkanRenderer* rend = raw_ptr(renderer);
    if (!out || !rend) {
        error = "null output or renderer";
        return nullptr;
    }
    auto impl = std::make_unique<FrameShim::Impl>();
    impl->frame = std::make_unique<lm::Frame>(*out, *rend);
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

// ============================================================================
// DirectScanout
// ============================================================================

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

// ============================================================================
// Scene hit testing and routing
// ============================================================================

namespace {
SceneHitRaw hit_raw(const lm::SceneHit& hit) {
    SceneHitRaw out{};
    out.hit = hit.hit;
    out.tag = hit.tag;
    out.surface_id = hit.surface ? u64_from_id(hit.surface->id()) : 0;
    out.sx = hit.sx;
    out.sy = hit.sy;
    return out;
}
} // namespace

SceneHitRaw raw_scene_route_pointer(const SeatShim& seat, rust::Slice<const SceneItemRaw> items, double x, double y) {
    lm::Seat* s = raw_ptr(seat);
    if (s == nullptr) return SceneHitRaw{};
    const std::vector<lm::SceneItem> scene = scene_items(items);
    return hit_raw(lm::scene_route_pointer(*s, scene, x, y));
}

bool raw_scene_surface_origin(rust::Slice<const SceneItemRaw> items, std::uint64_t surface_id, double& x, double& y) {
    const std::vector<lm::SceneItem> scene = scene_items(items);
    const auto origin = lm::scene_surface_origin(scene, id_from_u64(surface_id));
    if (!origin) return false;
    x = origin->x;
    y = origin->y;
    return true;
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
// PopupTree
// ============================================================================

struct PopupTreeShim::Impl {
    explicit Impl(lm::XdgShell& shell) : tree(shell) {}
    lm::PopupTree tree;
};
PopupTreeShim::~PopupTreeShim() = default;
PopupTreeShim::PopupTreeShim(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PopupTreeShim::Impl& PopupTreeShim::impl() const { return *impl_; }

std::unique_ptr<PopupTreeShim> popup_tree_new(const XdgShellShim& shell) {
    lm::XdgShell* s = raw_ptr(shell);
    if (s == nullptr) return nullptr;
    return std::make_unique<PopupTreeShim>(std::make_unique<PopupTreeShim::Impl>(*s));
}
bool PopupTreeShim::set_parent(const XdgShellShim& shell, std::uint64_t popup, std::uint64_t parent) const {
    lm::Popup* p = xdg_popup(shell, popup);
    if (p == nullptr) return false;
    impl_->tree.set_parent(*p, id_from_u64(parent));
    return true;
}
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

// ============================================================================
// LayerManager
// ============================================================================

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
    lm::LayerSurface* ls = layer_surface(shell, surface_id);
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
