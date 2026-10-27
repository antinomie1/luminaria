// luminaria_rs.h — the plain header the core Rust binding stands on.
//
// cxx's generated glue is compiled by the `cc` crate, which knows nothing about
// C++ modules or BMIs, so it has to be able to #include something. This is it:
// no `import`, no luminaria type, nothing but opaque pimpls and plain signatures.
// Everything that touches the core module lives in luminaria_rs.cpp, which xmake compiles.
#pragma once

#include <cstdint>
#include <memory>

#include "rust/cxx.h"

namespace lum {

// Shared structs from lib.rs.h
struct Mode;
struct Rect;
struct SceneItemRaw;
struct SceneHitRaw;
struct SurfaceAtRaw;
struct InputEventRaw;
struct EventRaw;
struct ToplevelInfoRaw;
struct PopupInfoRaw;
struct PopupAreaRaw;
struct LayerSurfaceInfoRaw;
struct IdleNotificationInfoRaw;
struct PointerConstraintInfoRaw;
struct TextInputInfoRaw;

class OutputGlobalShim;
class OutputShim;
class OutputLayoutShim;
class SeatShim;

// ============================================================================
// Core & Util
// ============================================================================

class DisplayShim {
public:
    ~DisplayShim();
    DisplayShim(const DisplayShim&) = delete;
    DisplayShim& operator=(const DisplayShim&) = delete;

    int fd() const;
    rust::String add_socket_auto(rust::String& error) const;
    bool add_socket(rust::Str name, rust::String& error) const;
    bool init_shm(rust::String& error) const;
    void run() const;
    void terminate() const;
    int dispatch(int timeout_ms) const;
    void flush_clients() const;
    // The next queued event of every shim on this thread, oldest first: 0 for
    // none, 1 an event in `out`, 2 an input event in `input`, 3 an output
    // added (`out.id`), which take_output() then hands over.
    std::uint32_t next_event(EventRaw& out, InputEventRaw& input) const;
    std::unique_ptr<OutputShim> take_output() const;
    // The wl_display, borrowed: see Display::as_ptr in src/core.rs.
    std::size_t raw_display() const;

    struct Impl;
    Impl& impl() const;
    explicit DisplayShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<DisplayShim> display_new(rust::String& error);

class KeymapStateShim {
public:
    ~KeymapStateShim();
    KeymapStateShim(const KeymapStateShim&) = delete;
    KeymapStateShim& operator=(const KeymapStateShim&) = delete;

    rust::String text() const;
    std::uint32_t key_keysym(std::uint32_t code) const;
    void update_key(std::uint32_t code, bool pressed);
    std::uint32_t modifiers_depressed() const;
    std::uint32_t modifiers_latched() const;
    std::uint32_t modifiers_locked() const;
    std::uint32_t modifiers_group() const;

    struct Impl;
    Impl& impl() const;
    explicit KeymapStateShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<KeymapStateShim> keymap_state_from_layout(rust::Str layout, rust::String& error);
std::unique_ptr<KeymapStateShim> keymap_state_from_text(rust::Str text, rust::String& error);

class RegionShim {
public:
    ~RegionShim();
    RegionShim(const RegionShim&) = delete;
    RegionShim& operator=(const RegionShim&) = delete;

    void add(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);
    void subtract_box(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);
    void subtract_region(const RegionShim& other);
    void intersect_box(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);
    void intersect_region(const RegionShim& other);
    bool empty() const;
    void clear();
    bool contains(std::int32_t x, std::int32_t y) const;
    Rect extents() const;
    rust::Vec<Rect> rects() const;

    struct Impl;
    Impl& impl() const;
    explicit RegionShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<RegionShim> region_new();
std::unique_ptr<RegionShim> region_from_box(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);

bool layout_fits_raw(std::int32_t width, std::int32_t height, std::uint32_t stride,
                     std::uint32_t offset, std::size_t total_size);

// ============================================================================
// Backend & Output (Core: Output, Headless, Wayland)
// ============================================================================

class OutputShim {
public:
    ~OutputShim();
    OutputShim(const OutputShim&) = delete;
    OutputShim& operator=(const OutputShim&) = delete;

    std::int32_t width() const;
    std::int32_t height() const;
    std::int32_t scale() const;
    void set_scale(std::int32_t scale);
    std::uint32_t transform() const;
    void set_transform(std::uint32_t transform);
    std::int32_t logical_width() const;
    std::int32_t logical_height() const;

    void schedule_frame();
    bool frame_scheduled() const;

    rust::Vec<Mode> modes() const;
    Mode current_mode() const;
    bool set_mode(std::int32_t width, std::int32_t height, std::int32_t refresh_mhz, rust::String& error);

    bool commit(float r, float g, float b, float a, rust::String& error);
    bool commit_frame(rust::Slice<const std::uint8_t> rgba, std::int32_t width, std::int32_t height, rust::String& error);

    rust::Vec<std::uint64_t> scanout_modifiers(std::uint32_t drm_format) const;
    bool has_cursor_plane() const;
    bool move_cursor(std::int32_t x, std::int32_t y, rust::String& error);
    bool hide_cursor(rust::String& error);
    bool alive() const;
    std::uint64_t id() const;

    struct Impl;
    Impl& impl() const;
    explicit OutputShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

class HeadlessBackendShim {
public:
    ~HeadlessBackendShim();
    HeadlessBackendShim(const HeadlessBackendShim&) = delete;
    HeadlessBackendShim& operator=(const HeadlessBackendShim&) = delete;

    std::unique_ptr<OutputShim> add_output(std::int32_t width, std::int32_t height);
    bool start(rust::String& error);

    struct Impl;
    Impl& impl() const;
    explicit HeadlessBackendShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<HeadlessBackendShim> headless_backend_new(const DisplayShim& display, unsigned int interval_ms, rust::String& error);

class WaylandBackendShim {
public:
    ~WaylandBackendShim();
    WaylandBackendShim(const WaylandBackendShim&) = delete;
    WaylandBackendShim& operator=(const WaylandBackendShim&) = delete;

    std::unique_ptr<OutputShim> add_output(std::int32_t width, std::int32_t height);
    bool start(rust::String& error);
    rust::String keymap() const;
    // keyboard, pointer, touch as bits 0, 1, 2.
    std::uint32_t capabilities() const;
    // lm::Seat::follow on this backend's input.
    void follow_seat(const SeatShim& seat, bool touch) const;

    struct Impl;
    Impl& impl() const;
    explicit WaylandBackendShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<WaylandBackendShim> wayland_backend_new(const DisplayShim& display, rust::String& error);

// ============================================================================
// Core Protocols
// ============================================================================

class CompositorShim {
public:
    ~CompositorShim();
    CompositorShim(const CompositorShim&) = delete;
    CompositorShim& operator=(const CompositorShim&) = delete;


    struct Impl;
    Impl& impl() const;
    explicit CompositorShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<CompositorShim> compositor_new(const DisplayShim& display, rust::String& error);

std::int32_t surface_width(std::uint64_t surface_id);
std::int32_t surface_height(std::uint64_t surface_id);
std::int32_t buffer_width(std::uint64_t surface_id);
std::int32_t buffer_height(std::uint64_t surface_id);
std::int32_t buffer_scale(std::uint64_t surface_id);
std::uint32_t buffer_transform(std::uint64_t surface_id);
bool surface_has_buffer(std::uint64_t surface_id);
rust::Vec<Rect> surface_damage(std::uint64_t surface_id);
void surface_send_frame_done(std::uint64_t surface_id, std::uint32_t time_ms);
void surface_notify_rendered(std::uint64_t surface_id, int fence_fd);
bool surface_accepts_input(std::uint64_t surface_id, double sx, double sy);
// Back to front, subsurfaces included; each entry's x/y is its offset from the
// root surface, so a grab on a subsurface can be placed where it is drawn.
rust::Vec<SurfaceAtRaw> surface_tree(std::uint64_t surface_id);
void surface_set_preferred_buffer_scale(std::uint64_t surface_id, std::int32_t scale);
void surface_set_preferred_buffer_transform(std::uint64_t surface_id, std::uint32_t transform);

class SubcompositorShim {
public:
    ~SubcompositorShim();
    SubcompositorShim(const SubcompositorShim&) = delete;
    SubcompositorShim& operator=(const SubcompositorShim&) = delete;

    struct Impl;
    Impl& impl() const;
    explicit SubcompositorShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<SubcompositorShim> subcompositor_new(const DisplayShim& display, rust::String& error);

class XdgShellShim {
public:
    ~XdgShellShim();
    XdgShellShim(const XdgShellShim&) = delete;
    XdgShellShim& operator=(const XdgShellShim&) = delete;

    void set_bounds(std::int32_t width, std::int32_t height) const;
    // From now on, maximize/fullscreen/minimize requests of toplevels created
    // later are Rust's to answer; until then the library grants them itself.
    void forward_window_requests() const;
    rust::Vec<std::uint64_t> toplevels() const;

    // Toplevels and popups are named by their wl_surface's SurfaceId and
    // looked up again on every call; a stale id finds nothing.
    bool toplevel_info(std::uint64_t surface_id, ToplevelInfoRaw& out) const;
    rust::String toplevel_title(std::uint64_t surface_id) const;
    rust::String toplevel_app_id(std::uint64_t surface_id) const;
    std::uint32_t toplevel_configure(std::uint64_t surface_id, std::int32_t width, std::int32_t height) const;
    // `state`: one of the ToplevelStates bits in src/protocol/xdg_shell.rs.
    void toplevel_set_state(std::uint64_t surface_id, std::uint32_t state, bool on) const;
    void toplevel_close(std::uint64_t surface_id) const;

    bool popup_info(std::uint64_t surface_id, PopupInfoRaw& out) const;
    void popup_dismiss(std::uint64_t surface_id) const;
    void set_popup_constraint_areas(rust::Slice<const PopupAreaRaw> areas) const;

    struct Impl;
    Impl& impl() const;
    explicit XdgShellShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<XdgShellShim> xdg_shell_new(const DisplayShim& display, rust::String& error);

class SeatShim {
public:
    ~SeatShim();
    SeatShim(const SeatShim&) = delete;
    SeatShim& operator=(const SeatShim&) = delete;

    void set_capabilities(bool keyboard, bool pointer, bool touch) const;
    bool set_keymap(rust::Str xkb_text) const;
    rust::String keymap() const;

    void set_keyboard_focus(std::uint64_t surface_id) const;
    std::uint64_t keyboard_focus() const;
    void notify_key(std::uint32_t key, bool pressed) const;
    void notify_modifiers(std::uint32_t depressed, std::uint32_t latched,
                          std::uint32_t locked, std::uint32_t group) const;
    void release_keys() const;

    void pointer_enter(std::uint64_t surface_id, double sx, double sy) const;
    void pointer_clear_focus() const;
    std::uint64_t pointer_focus() const;
    std::uint64_t pointer_grab() const;
    void pointer_motion(double sx, double sy) const;
    void pointer_button(std::uint32_t button, bool pressed) const;
    void pointer_axis(double dx, double dy) const;
    void pointer_axis_discrete(std::int32_t dx_steps, std::int32_t dy_steps) const;
    void pointer_axis_stop(bool horizontal, bool vertical) const;
    void pointer_axis_event(double dx, double dy, std::int32_t dx_steps, std::int32_t dy_steps,
                            bool stop_horizontal, bool stop_vertical) const;

    std::uint64_t cursor_surface() const;
    bool cursor_hidden() const;
    std::int32_t cursor_hotspot_x() const;
    std::int32_t cursor_hotspot_y() const;

    void touch_down(std::uint64_t surface_id, std::int32_t id, double x, double y) const;
    void touch_motion(std::int32_t id, double x, double y) const;
    void touch_up(std::int32_t id) const;
    void touch_frame() const;
    void touch_cancel() const;
    std::uint64_t touch_focus(std::int32_t id) const;


    struct Impl;
    Impl& impl() const;
    explicit SeatShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<SeatShim> seat_new(const DisplayShim& display, rust::Str name, rust::String& error);

class LayerShellShim {
public:
    ~LayerShellShim();
    LayerShellShim(const LayerShellShim&) = delete;
    LayerShellShim& operator=(const LayerShellShim&) = delete;

    rust::Vec<std::uint64_t> layer_surfaces() const;
    bool info(std::uint64_t surface_id, LayerSurfaceInfoRaw& out) const;
    rust::String scope(std::uint64_t surface_id) const;
    bool on_output(std::uint64_t surface_id, const OutputGlobalShim& output) const;
    std::uint32_t configure(std::uint64_t surface_id, std::int32_t width, std::int32_t height) const;
    void close(std::uint64_t surface_id) const;
    Rect arrange(std::uint64_t surface_id, Rect full, Rect& usable) const;

    struct Impl;
    Impl& impl() const;
    explicit LayerShellShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<LayerShellShim> layer_shell_new(const DisplayShim& display, rust::String& error);

class OutputGlobalShim {
public:
    ~OutputGlobalShim();
    OutputGlobalShim(const OutputGlobalShim&) = delete;
    OutputGlobalShim& operator=(const OutputGlobalShim&) = delete;

    void set_logical_position(std::int32_t x, std::int32_t y) const;
    void set_mode(std::int32_t width, std::int32_t height, std::int32_t refresh_mhz) const;
    void set_scale(std::int32_t scale) const;
    void set_transform(std::uint32_t transform) const;

    std::int32_t logical_x() const;
    std::int32_t logical_y() const;
    std::int32_t logical_width() const;
    std::int32_t logical_height() const;
    std::int32_t width() const;
    std::int32_t height() const;
    std::int32_t scale() const;
    std::uint32_t transform() const;
    std::int32_t refresh_mhz() const;
    rust::String name() const;

    struct Impl;
    Impl& impl() const;
    explicit OutputGlobalShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<OutputGlobalShim> output_global_new(const DisplayShim& display, std::int32_t width,
                                                   std::int32_t height, rust::Str name, rust::String& error);
// lm::OutputGlobal::create(display, output, layout, name): follows both.
std::unique_ptr<OutputGlobalShim> output_global_for(const DisplayShim& display, const OutputShim& output,
                                                   const OutputLayoutShim& layout, rust::Str name,
                                                   rust::String& error);

class CursorThemeShim {
public:
    ~CursorThemeShim();
    CursorThemeShim(const CursorThemeShim&) = delete;
    CursorThemeShim& operator=(const CursorThemeShim&) = delete;

    struct Impl;
    Impl& impl() const;
    explicit CursorThemeShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<CursorThemeShim> cursor_theme_load(rust::Str name, std::int32_t size, rust::String& error);

// Protocol Managers
#define DECLARE_GLOBAL_SHIM(Name, FuncName) \
class Name##Shim { \
public: \
    ~Name##Shim(); \
    Name##Shim(const Name##Shim&) = delete; \
    Name##Shim& operator=(const Name##Shim&) = delete; \
    struct Impl; \
    Impl& impl() const; \
    explicit Name##Shim(std::unique_ptr<Impl> impl) noexcept; \
private: \
    std::unique_ptr<Impl> impl_; \
}; \
std::unique_ptr<Name##Shim> FuncName(const DisplayShim& display, rust::String& error);

DECLARE_GLOBAL_SHIM(Viewporter, viewporter_new)
DECLARE_GLOBAL_SHIM(SinglePixelBufferManager, single_pixel_buffer_manager_new)
DECLARE_GLOBAL_SHIM(TearingControlManager, tearing_control_manager_new)
DECLARE_GLOBAL_SHIM(ContentTypeManager, content_type_manager_new)
DECLARE_GLOBAL_SHIM(FifoManager, fifo_manager_new)
DECLARE_GLOBAL_SHIM(CommitTimingManager, commit_timing_manager_new)
DECLARE_GLOBAL_SHIM(Presentation, presentation_new)
DECLARE_GLOBAL_SHIM(BackgroundEffectManager, background_effect_manager_new)

#undef DECLARE_GLOBAL_SHIM

// The standard members of a hand-written shim.
#define LUM_SHIM_BOILERPLATE(Name) \
    ~Name(); \
    Name(const Name&) = delete; \
    Name& operator=(const Name&) = delete; \
    struct Impl; \
    Impl& impl() const; \
    explicit Name(std::unique_ptr<Impl> impl) noexcept;

class XdgDecorationManagerShim {
public:
    LUM_SHIM_BOILERPLATE(XdgDecorationManagerShim)
    void set_default_mode(std::uint32_t mode) const;
    std::uint32_t default_mode() const;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<XdgDecorationManagerShim> xdg_decoration_manager_new(const DisplayShim& display, rust::String& error);

class FractionalScaleManagerShim {
public:
    LUM_SHIM_BOILERPLATE(FractionalScaleManagerShim)
    // lm::FractionalScaleManager::set_scale: the exact scale and the integer hint.
    void set_scale(std::uint64_t surface_id, double scale) const;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<FractionalScaleManagerShim> fractional_scale_manager_new(const DisplayShim& display, rust::String& error);

class CursorShapeManagerShim {
public:
    LUM_SHIM_BOILERPLATE(CursorShapeManagerShim)
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<CursorShapeManagerShim> cursor_shape_manager_new(const DisplayShim& display, rust::String& error);

class XdgToplevelIconManagerShim {
public:
    LUM_SHIM_BOILERPLATE(XdgToplevelIconManagerShim)
    rust::String icon_name(std::uint64_t surface_id) const;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<XdgToplevelIconManagerShim> xdg_toplevel_icon_manager_new(const DisplayShim& display, rust::String& error);

class XdgActivationShim {
public:
    LUM_SHIM_BOILERPLATE(XdgActivationShim)
    void set_grant_tokens(bool grant) const;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<XdgActivationShim> xdg_activation_new(const DisplayShim& display, rust::String& error);

class IdleInhibitManagerShim {
public:
    LUM_SHIM_BOILERPLATE(IdleInhibitManagerShim)
    bool inhibited() const;
    std::size_t inhibitor_count() const;
    std::uint64_t inhibitor_surface(std::uint64_t inhibitor) const;
    bool inhibitor_visible(std::uint64_t inhibitor) const;
    void set_inhibitor_visible(std::uint64_t inhibitor, bool visible) const;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<IdleInhibitManagerShim> idle_inhibit_manager_new(const DisplayShim& display, rust::String& error);

class IdleNotifierShim {
public:
    LUM_SHIM_BOILERPLATE(IdleNotifierShim)
    void notify_activity() const;
    void set_inhibited(bool inhibited) const;
    bool inhibited() const;
    std::size_t notification_count() const;
    std::size_t idled_count() const;
    bool notification_info(std::uint64_t notification, IdleNotificationInfoRaw& out) const;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<IdleNotifierShim> idle_notifier_new(const DisplayShim& display, rust::String& error);

class DataDeviceManagerShim {
public:
    ~DataDeviceManagerShim();
    DataDeviceManagerShim(const DataDeviceManagerShim&) = delete;
    DataDeviceManagerShim& operator=(const DataDeviceManagerShim&) = delete;
    rust::Vec<rust::String> selection_mime_types() const;
    // Takes ownership of `fd` either way.
    bool selection_receive(rust::Str mime, int fd) const;
    struct Impl;
    Impl& impl() const;
    explicit DataDeviceManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<DataDeviceManagerShim> data_device_manager_new(const DisplayShim& display, const SeatShim& seat, rust::String& error);

class PrimarySelectionManagerShim {
public:
    ~PrimarySelectionManagerShim();
    PrimarySelectionManagerShim(const PrimarySelectionManagerShim&) = delete;
    PrimarySelectionManagerShim& operator=(const PrimarySelectionManagerShim&) = delete;
    rust::Vec<rust::String> selection_mime_types() const;
    // Takes ownership of `fd` either way.
    bool selection_receive(rust::Str mime, int fd) const;
    struct Impl;
    Impl& impl() const;
    explicit PrimarySelectionManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<PrimarySelectionManagerShim> primary_selection_manager_new(const DisplayShim& display, const SeatShim& seat, rust::String& error);

class RelativePointerManagerShim {
public:
    ~RelativePointerManagerShim();
    RelativePointerManagerShim(const RelativePointerManagerShim&) = delete;
    RelativePointerManagerShim& operator=(const RelativePointerManagerShim&) = delete;
    struct Impl;
    Impl& impl() const;
    explicit RelativePointerManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<RelativePointerManagerShim> relative_pointer_manager_new(const DisplayShim& display, const SeatShim& seat, rust::String& error);

class PointerConstraintsShim {
public:
    ~PointerConstraintsShim();
    PointerConstraintsShim(const PointerConstraintsShim&) = delete;
    PointerConstraintsShim& operator=(const PointerConstraintsShim&) = delete;
    // Constraints are named by the surface they constrain: one per surface.
    bool constraint_info(std::uint64_t surface_id, PointerConstraintInfoRaw& out) const;
    rust::Vec<Rect> constraint_region(std::uint64_t surface_id) const;
    void activate(std::uint64_t surface_id) const;
    void deactivate(std::uint64_t surface_id) const;
    std::uint64_t active_constraint() const;
    struct Impl;
    Impl& impl() const;
    explicit PointerConstraintsShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<PointerConstraintsShim> pointer_constraints_new(const DisplayShim& display, const SeatShim& seat, rust::String& error);

class TextInputManagerShim {
public:
    ~TextInputManagerShim();
    TextInputManagerShim(const TextInputManagerShim&) = delete;
    TextInputManagerShim& operator=(const TextInputManagerShim&) = delete;
    std::uint64_t focused() const;
    bool text_input_info(std::uint64_t text_input, TextInputInfoRaw& out) const;
    rust::String surrounding_text(std::uint64_t text_input) const;
    void send_preedit_string(std::uint64_t text_input, rust::Str text, std::int32_t cursor_begin, std::int32_t cursor_end) const;
    void send_commit_string(std::uint64_t text_input, rust::Str text) const;
    void send_delete_surrounding_text(std::uint64_t text_input, std::uint32_t before, std::uint32_t after) const;
    void send_done(std::uint64_t text_input) const;
    struct Impl;
    Impl& impl() const;
    explicit TextInputManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<TextInputManagerShim> text_input_manager_new(const DisplayShim& display, const SeatShim& seat, rust::String& error);

// ============================================================================
// Core Shell
// ============================================================================

class OutputLayoutShim {
public:
    ~OutputLayoutShim();
    OutputLayoutShim(const OutputLayoutShim&) = delete;
    OutputLayoutShim& operator=(const OutputLayoutShim&) = delete;

    void add(OutputShim& output, std::int32_t x, std::int32_t y) const;
    void add_auto(OutputShim& output) const;
    void remove(OutputShim& output) const;
    Rect bounds() const;
    Rect output_box(const OutputShim& output) const;

    struct Impl;
    Impl& impl() const;
    explicit OutputLayoutShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<OutputLayoutShim> output_layout_new();

class CpuCompositorShim {
public:
    ~CpuCompositorShim();
    CpuCompositorShim(const CpuCompositorShim&) = delete;
    CpuCompositorShim& operator=(const CpuCompositorShim&) = delete;

    void composite_rect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                        float r, float g, float b, float a);
    rust::Vec<std::uint8_t> finish(std::int32_t width, std::int32_t height,
                                  float bg_r, float bg_g, float bg_b, float bg_a);

    struct Impl;
    Impl& impl() const;
    explicit CpuCompositorShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<CpuCompositorShim> cpu_compositor_new();

SceneHitRaw raw_scene_hit_test(rust::Slice<const SceneItemRaw> items, double x, double y);
SceneHitRaw raw_scene_route_pointer(const SeatShim& seat, rust::Slice<const SceneItemRaw> items, double x, double y);
bool raw_scene_surface_origin(rust::Slice<const SceneItemRaw> items, std::uint64_t surface_id, double& x, double& y);

class PopupTreeShim {
public:
    ~PopupTreeShim();
    PopupTreeShim(const PopupTreeShim&) = delete;
    PopupTreeShim& operator=(const PopupTreeShim&) = delete;

    // For a popup whose parent is not an xdg surface (a layer surface's).
    bool set_parent(const XdgShellShim& shell, std::uint64_t popup, std::uint64_t parent) const;
    // Only the items appended to `scene`.
    rust::Vec<SceneItemRaw> append_to(rust::Slice<const SceneItemRaw> scene) const;
    bool contains(std::uint64_t surface_id) const;
    std::uint64_t active_grab() const;
    bool dismiss_grabs_outside(std::uint64_t surface_id);

    struct Impl;
    Impl& impl() const;
    explicit PopupTreeShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<PopupTreeShim> popup_tree_new(const XdgShellShim& shell);

class LayerManagerShim {
public:
    LUM_SHIM_BOILERPLATE(LayerManagerShim)
    bool add(const LayerShellShim& shell, std::uint64_t surface_id, std::uint32_t output_id);
    void remove(std::uint64_t surface_id);
    void set_mapped(std::uint64_t surface_id, bool mapped);
    Rect arrange(std::uint32_t output_id, Rect full, Rect usable);
    rust::Vec<SceneItemRaw> append_to(std::uint32_t output_id, std::uint32_t layer) const;
    std::uint64_t keyboard_target(std::uint64_t surface_id) const;
    void close_output(std::uint32_t output_id);
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<LayerManagerShim> layer_manager_new();

#undef LUM_SHIM_BOILERPLATE

} // namespace lum
