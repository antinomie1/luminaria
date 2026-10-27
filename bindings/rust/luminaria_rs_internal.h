// luminaria_rs_internal.h — shared helpers and core shim accessors for the C++ shim TUs.
// Included AFTER `import luminaria;` in each shim .cpp file.
#pragma once

#include <wayland-server-core.h>

namespace lum {

namespace lm = luminaria;

template <class E>
using Conn = typename lm::Signal<E>::Connection;

// Client strings are not guaranteed UTF-8, and rust::String's checked
// constructor throws on bad input. Lossy never throws.
inline rust::String to_rust(const std::string& s) { return rust::String::lossy(s); }

inline rust::Vec<rust::String> to_rust(const std::vector<std::string>& v) {
    rust::Vec<rust::String> out;
    for (const auto& s : v) out.push_back(to_rust(s));
    return out;
}

inline Rect to_rect(const lm::Box& b) { return Rect{b.x, b.y, b.width, b.height}; }
inline lm::Box to_box(const Rect& r) { return lm::Box{r.x, r.y, r.width, r.height}; }

inline lm::SurfaceId id_from_u64(std::uint64_t raw) {
    if (raw == 0) return lm::SurfaceId{};
    return lm::SurfaceId{
        static_cast<std::uint32_t>(raw & 0xFFFFFFFFu),
        static_cast<std::uint32_t>(raw >> 32),
    };
}

inline std::uint64_t u64_from_id(lm::SurfaceId id) {
    return id.valid() ? ((static_cast<std::uint64_t>(id.generation) << 32) | id.index) : 0;
}

inline std::uint64_t surface_u64(lm::Surface* s) { return s != nullptr ? u64_from_id(s->id()) : 0; }

inline std::uint32_t caps_bits(lm::InputCapabilities c) {
    return (c.keyboard ? 1u : 0u) | (c.pointer ? 2u : 0u) | (c.touch ? 4u : 0u);
}

// One `lm::InputEvent` for Rust, whichever source emitted it. Absolute motion
// leaves `output` 0: only the source knows which `OutputShim` id its output got.
inline InputEventRaw input_event_raw(const lm::InputEvent& event) {
    InputEventRaw r{};
    r.pressed = false;
    std::visit(
        [&r](const auto& e) {
            using E = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<E, lm::KeyEvent>) {
                r.kind = InputEventKind::Key;
                r.code = e.keycode;
                r.pressed = e.pressed;
                r.keysym = e.keysym;
                r.depressed = e.modifiers.depressed;
                r.latched = e.modifiers.latched;
                r.locked = e.modifiers.locked;
                r.group = e.modifiers.group;
            } else if constexpr (std::is_same_v<E, lm::ModifiersEvent>) {
                r.kind = InputEventKind::Modifiers;
                r.depressed = e.depressed;
                r.latched = e.latched;
                r.locked = e.locked;
                r.group = e.group;
            } else if constexpr (std::is_same_v<E, lm::KeymapChange>) {
                r.kind = InputEventKind::KeymapChange;
                r.text = rust::String::lossy(e.text.data(), e.text.size());
            } else if constexpr (std::is_same_v<E, lm::PointerMotionEvent>) {
                r.kind = InputEventKind::Motion;
                r.x = e.dx;
                r.y = e.dy;
            } else if constexpr (std::is_same_v<E, lm::PointerMotionAbsEvent>) {
                r.kind = InputEventKind::MotionAbsolute;
                r.x = e.x;
                r.y = e.y;
            } else if constexpr (std::is_same_v<E, lm::PointerLeaveEvent>) {
                r.kind = InputEventKind::PointerLeave;
            } else if constexpr (std::is_same_v<E, lm::PointerButtonEvent>) {
                r.kind = InputEventKind::Button;
                r.code = e.button;
                r.pressed = e.pressed;
            } else if constexpr (std::is_same_v<E, lm::PointerAxisEvent>) {
                r.kind = InputEventKind::Axis;
                r.x = e.dx;
                r.y = e.dy;
                r.steps_x = e.dx_steps;
                r.steps_y = e.dy_steps;
                r.flags = (e.stop_horizontal ? 1u : 0u) | (e.stop_vertical ? 2u : 0u);
            } else if constexpr (std::is_same_v<E, lm::TouchDownEvent>) {
                r.kind = InputEventKind::TouchDown;
                r.id = e.id;
                r.x = e.x;
                r.y = e.y;
            } else if constexpr (std::is_same_v<E, lm::TouchMotionEvent>) {
                r.kind = InputEventKind::TouchMotion;
                r.id = e.id;
                r.x = e.x;
                r.y = e.y;
            } else if constexpr (std::is_same_v<E, lm::TouchUpEvent>) {
                r.kind = InputEventKind::TouchUp;
                r.id = e.id;
            } else if constexpr (std::is_same_v<E, lm::TouchFrameEvent>) {
                r.kind = InputEventKind::TouchFrame;
            } else if constexpr (std::is_same_v<E, lm::TouchCancelEvent>) {
                r.kind = InputEventKind::TouchCancel;
                r.id = e.id;
            } else {
                static_assert(std::is_same_v<E, lm::InputCapabilities>);
                r.kind = InputEventKind::Capabilities;
                r.flags = caps_bits(e);
            }
        },
        event);
    return r;
}

// An event for Rust to drain; `kind` says which of the other fields mean
// something. Every shim that forwards signals keeps one queue of these.
inline EventRaw event(EventKind kind, std::uint64_t id = 0, std::uint64_t other = 0) {
    EventRaw e{};
    e.kind = kind;
    e.id = id;
    e.other = other;
    e.flag = false;
    e.flag2 = false;
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

struct StoredImage {
    int width = 0;
    int height = 0;
    std::vector<lm::Pixel> pixels;
};

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

// Identity for an object that has no surface of its own to be named by (a
// lock, an idle notification, a text input). Never reused.
std::uint64_t next_object_id();

// Whether `resource` (a client's wl_output) is `global`'s.
bool is_output(wl_resource* resource, const OutputGlobalShim& global);

// Cross-TU accessors for core shims:
lm::Display* raw_ptr(const DisplayShim& s);
lm::Output* raw_ptr(const OutputShim& s);
std::unique_ptr<OutputShim> make_output_shim(lm::Output* output);
lm::OutputLayout& raw_ref(const OutputLayoutShim& s);
lm::Seat* raw_ptr(const SeatShim& s);
lm::XdgShell* raw_ptr(const XdgShellShim& s);
lm::CursorShapeManager* raw_ptr(const CursorShapeManagerShim& s);
lm::DataDeviceManager* raw_ptr(const DataDeviceManagerShim& s);
lm::TextInputManager* raw_ptr(const TextInputManagerShim& s);

struct PresentationObserver {
    virtual ~PresentationObserver() = default;
    virtual void on_presentation_destroyed() = 0;
};

struct PresentationShim::Impl {
    std::optional<lm::Presentation> mgr;
    std::vector<PresentationObserver*> observers;

    // Before `mgr` goes: no renderer may still be pointing at it.
    ~Impl() {
        for (PresentationObserver* obs : observers) {
            obs->on_presentation_destroyed();
        }
    }
};

} // namespace lum
