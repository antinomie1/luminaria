// luminaria_rs.h — the plain header the Rust binding stands on.
//
// cxx's generated glue is compiled by the `cc` crate, which knows nothing about
// C++ modules or BMIs, so it has to be able to #include something. This is it:
// no `import`, no luminaria type, nothing but opaque pimpls and plain signatures.
// Everything that touches the modules lives in luminaria_rs.cpp, which xmake compiles.
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
struct ToplevelEventRaw;
struct BackendInputEventRaw;
struct LibinputEventRaw;
struct DrmDeviceId;
struct VulkanHandlesRaw;

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
// Backend & Output
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
    bool take_frame();
    bool take_mode_changed();
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
    bool next_input_event(BackendInputEventRaw& out);

    struct Impl;
    Impl& impl() const;
    explicit WaylandBackendShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<WaylandBackendShim> wayland_backend_new(const DisplayShim& display, rust::String& error);

class SessionShim {
public:
    ~SessionShim();
    SessionShim(const SessionShim&) = delete;
    SessionShim& operator=(const SessionShim&) = delete;

    bool switch_to(int vt, rust::String& error) const;
    bool active() const;
    rust::String seat_name() const;
    // 0 = nothing queued, 1 = became inactive, 2 = became active.
    std::uint32_t next_activity() const;

    struct Impl;
    Impl& impl() const;
    explicit SessionShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<SessionShim> session_new(const DisplayShim& display, rust::String& error);

class DrmBackendShim {
public:
    ~DrmBackendShim();
    DrmBackendShim(const DrmBackendShim&) = delete;
    DrmBackendShim& operator=(const DrmBackendShim&) = delete;

    bool start(rust::String& error);
    int fd() const;
    void scan_connectors();
    // kind: 0 = nothing queued, 1 = output added (returned), 2 = removed (`removed` set).
    std::unique_ptr<OutputShim> next_event(std::uint32_t& kind, std::uint64_t& removed);

    struct Impl;
    Impl& impl() const;
    explicit DrmBackendShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<DrmBackendShim> drm_backend_new(const DisplayShim& display, rust::Str device, const SessionShim& session, rust::String& error);
std::unique_ptr<DrmBackendShim> drm_backend_new_standalone(const DisplayShim& display, rust::Str device, rust::String& error);

class LibinputBackendShim {
public:
    ~LibinputBackendShim();
    LibinputBackendShim(const LibinputBackendShim&) = delete;
    LibinputBackendShim& operator=(const LibinputBackendShim&) = delete;

    bool start(rust::String& error);
    bool add_device(rust::Str path, rust::String& error);
    bool next_input_event(LibinputEventRaw& out);
    rust::String keymap() const;
    bool set_keymap(rust::Str xkb_text);
    // keyboard, pointer, touch as bits 0, 1, 2.
    std::uint32_t capabilities() const;

    struct Impl;
    Impl& impl() const;
    explicit LibinputBackendShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<LibinputBackendShim> libinput_backend_new(const DisplayShim& display, const SessionShim& session, rust::String& error);
std::unique_ptr<LibinputBackendShim> libinput_backend_new_standalone(const DisplayShim& display, rust::String& error);
std::unique_ptr<LibinputBackendShim> libinput_backend_new_path(const DisplayShim& display, const SessionShim& session, rust::String& error);
std::unique_ptr<LibinputBackendShim> libinput_backend_new_path_standalone(const DisplayShim& display, rust::String& error);

// ============================================================================
// Protocols
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
bool surface_accepts_input(std::uint64_t surface_id, double sx, double sy);

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
    bool next_toplevel_event(ToplevelEventRaw& out) const;
    void toplevel_configure(std::uint64_t surface_id, std::int32_t width, std::int32_t height) const;
    void toplevel_set_maximized(std::uint64_t surface_id, bool on) const;
    void toplevel_set_activated(std::uint64_t surface_id, bool on) const;
    void toplevel_close(std::uint64_t surface_id) const;

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

    void pointer_enter(std::uint64_t surface_id, double sx, double sy) const;
    void pointer_clear_focus() const;
    std::uint64_t pointer_focus() const;
    std::uint64_t pointer_grab() const;
    void pointer_motion(double sx, double sy) const;
    void pointer_button(std::uint32_t button, bool pressed) const;
    void pointer_axis(double dx, double dy) const;
    void pointer_axis_discrete(std::int32_t dx_steps, std::int32_t dy_steps) const;
    void pointer_axis_stop(bool horizontal, bool vertical) const;

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

    struct Impl;
    Impl& impl() const;
    explicit OutputGlobalShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<OutputGlobalShim> output_global_new(const DisplayShim& display, std::int32_t width,
                                                   std::int32_t height, rust::Str name, rust::String& error);

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
DECLARE_GLOBAL_SHIM(FractionalScaleManager, fractional_scale_manager_new)
DECLARE_GLOBAL_SHIM(XdgActivation, xdg_activation_new)
DECLARE_GLOBAL_SHIM(XdgDecorationManager, xdg_decoration_manager_new)
DECLARE_GLOBAL_SHIM(CursorShapeManager, cursor_shape_manager_new)
DECLARE_GLOBAL_SHIM(IdleInhibitManager, idle_inhibit_manager_new)
DECLARE_GLOBAL_SHIM(IdleNotifier, idle_notifier_new)
DECLARE_GLOBAL_SHIM(SinglePixelBufferManager, single_pixel_buffer_manager_new)
DECLARE_GLOBAL_SHIM(TearingControlManager, tearing_control_manager_new)
DECLARE_GLOBAL_SHIM(ContentTypeManager, content_type_manager_new)
DECLARE_GLOBAL_SHIM(FifoManager, fifo_manager_new)
DECLARE_GLOBAL_SHIM(CommitTimingManager, commit_timing_manager_new)
DECLARE_GLOBAL_SHIM(Presentation, presentation_new)
DECLARE_GLOBAL_SHIM(BackgroundEffectManager, background_effect_manager_new)
DECLARE_GLOBAL_SHIM(XdgToplevelIconManager, xdg_toplevel_icon_manager_new)
DECLARE_GLOBAL_SHIM(DrmSyncobjManager, drm_syncobj_manager_new)
DECLARE_GLOBAL_SHIM(ScreencopyManager, screencopy_manager_new)
DECLARE_GLOBAL_SHIM(ForeignToplevelManager, foreign_toplevel_manager_new)
DECLARE_GLOBAL_SHIM(WorkspaceManager, workspace_manager_new)
DECLARE_GLOBAL_SHIM(SessionLockManager, session_lock_manager_new)

#undef DECLARE_GLOBAL_SHIM

class DataDeviceManagerShim {
public:
    ~DataDeviceManagerShim();
    DataDeviceManagerShim(const DataDeviceManagerShim&) = delete;
    DataDeviceManagerShim& operator=(const DataDeviceManagerShim&) = delete;
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
    struct Impl;
    Impl& impl() const;
    explicit TextInputManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<TextInputManagerShim> text_input_manager_new(const DisplayShim& display, const SeatShim& seat, rust::String& error);

// ============================================================================
// GPU & Render
// ============================================================================

class VulkanRendererShim {
public:
    ~VulkanRendererShim();
    VulkanRendererShim(const VulkanRendererShim&) = delete;
    VulkanRendererShim& operator=(const VulkanRendererShim&) = delete;

    rust::Vec<std::uint64_t> scanout_modifiers(std::uint32_t drm_format) const;
    VulkanHandlesRaw handles() const;

    struct Impl;
    Impl& impl() const;
    explicit VulkanRendererShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

bool drm_device_id_from_fd(int fd, DrmDeviceId& out);
bool drm_device_id_from_path(rust::Str path, DrmDeviceId& out);

// `has_drm_device` false: no DRM device to match, the ranking decides.
std::unique_ptr<VulkanRendererShim> vulkan_renderer_new(bool has_drm_device, const DrmDeviceId& drm_device, rust::String& error);

class LinuxDmabufShim {
public:
    ~LinuxDmabufShim();
    LinuxDmabufShim(const LinuxDmabufShim&) = delete;
    LinuxDmabufShim& operator=(const LinuxDmabufShim&) = delete;

    struct Impl;
    Impl& impl() const;
    explicit LinuxDmabufShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<LinuxDmabufShim> linux_dmabuf_new(const DisplayShim& display, const VulkanRendererShim& renderer, rust::String& error);
std::unique_ptr<LinuxDmabufShim> linux_dmabuf_new_headless(const DisplayShim& display, rust::String& error);

class SceneOutputShim {
public:
    ~SceneOutputShim();
    SceneOutputShim(const SceneOutputShim&) = delete;
    SceneOutputShim& operator=(const SceneOutputShim&) = delete;

    bool gpu_output() const;

    struct Impl;
    Impl& impl() const;
    explicit SceneOutputShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<SceneOutputShim> scene_output_new();

class SceneRendererShim {
public:
    ~SceneRendererShim();
    SceneRendererShim(const SceneRendererShim&) = delete;
    SceneRendererShim& operator=(const SceneRendererShim&) = delete;

    bool gpu_enabled() const;
    rust::String gpu_error() const;
    bool reset(SceneOutputShim& state, OutputShim& output, std::uint32_t drm_format, rust::String& error) const;
    int present(SceneOutputShim& state, OutputShim& output, rust::Slice<const SceneItemRaw> items,
                float bg_r, float bg_g, float bg_b, float bg_a, rust::String& error) const;
    void set_presentation(const PresentationShim& presentation) const;
    void clear_presentation() const;
    void update_cursor() const;
    rust::Vec<std::uint8_t> read_back(SceneOutputShim& state, rust::String& error) const;
    bool vulkan_handles(VulkanHandlesRaw& out) const;

    struct Impl;
    Impl& impl() const;
    explicit SceneRendererShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<SceneRendererShim> scene_renderer_new(bool has_drm_device, const DrmDeviceId& drm_device, rust::String& error);

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

class CursorShim {
public:
    ~CursorShim();
    CursorShim(const CursorShim&) = delete;
    CursorShim& operator=(const CursorShim&) = delete;

    bool move_to(double x, double y, Rect bounds);
    bool move_by(double dx, double dy, Rect bounds);
    void leave();
    void refresh();
    bool position(double& x, double& y) const;

    struct Impl;
    Impl& impl() const;
    explicit CursorShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<CursorShim> cursor_new(const SceneRendererShim& renderer, const OutputLayoutShim& layout,
                                      const SeatShim& seat, rust::String& error);

class FrameShim {
public:
    ~FrameShim();
    FrameShim(const FrameShim&) = delete;
    FrameShim& operator=(const FrameShim&) = delete;

    bool reset(std::uint32_t drm_format, rust::String& error);
    void begin(std::int32_t vx, std::int32_t vy, std::int32_t vw, std::int32_t vh);
    void place_rect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                    float r, float g, float b, float a);
    int submit(float bg_r, float bg_g, float bg_b, float bg_a, rust::String& error);
    void send_frame_done(std::uint32_t time_ms);

    struct Impl;
    Impl& impl() const;
    explicit FrameShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<FrameShim> frame_new(OutputShim& output, const VulkanRendererShim& renderer, rust::String& error);

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

class DirectScanoutShim {
public:
    ~DirectScanoutShim();
    DirectScanoutShim(const DirectScanoutShim&) = delete;
    DirectScanoutShim& operator=(const DirectScanoutShim&) = delete;

    bool available() const;
    std::uint32_t id_for(std::uint64_t surface_id);
    void committed(std::uint64_t surface_id);
    void presented();
    void clear();
    std::size_t cached() const;

    struct Impl;
    Impl& impl() const;
    explicit DirectScanoutShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<DirectScanoutShim> direct_scanout_new(OutputShim& output);

SceneHitRaw raw_scene_hit_test(rust::Slice<const SceneItemRaw> items, double x, double y);

// ============================================================================
// Desktop Protocols
// ============================================================================

void foreign_toplevel_track(ForeignToplevelManagerShim& ftm, const XdgShellShim& shell);

std::uint32_t workspace_manager_create_group(WorkspaceManagerShim& wm);
std::uint32_t workspace_manager_create_workspace(WorkspaceManagerShim& wm, std::uint32_t group, rust::Str name);
void workspace_manager_remove_workspace(WorkspaceManagerShim& wm, std::uint32_t workspace);
void workspace_manager_activate_workspace(WorkspaceManagerShim& wm, std::uint32_t workspace);

bool session_lock_manager_locked(SessionLockManagerShim& slm);

class OutputManagerShim {
public:
    ~OutputManagerShim();
    OutputManagerShim(const OutputManagerShim&) = delete;
    OutputManagerShim& operator=(const OutputManagerShim&) = delete;

    void update();

    struct Impl;
    Impl& impl() const;
    explicit OutputManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<OutputManagerShim> output_manager_new(const DisplayShim& display, const OutputLayoutShim& layout, rust::String& error);

class DataControlManagerShim {
public:
    ~DataControlManagerShim();
    DataControlManagerShim(const DataControlManagerShim&) = delete;
    DataControlManagerShim& operator=(const DataControlManagerShim&) = delete;

    struct Impl;
    Impl& impl() const;
    explicit DataControlManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<DataControlManagerShim> data_control_manager_new(const DisplayShim& display, const DataDeviceManagerShim& ddm, rust::String& error);

class InputMethodManagerShim {
public:
    ~InputMethodManagerShim();
    InputMethodManagerShim(const InputMethodManagerShim&) = delete;
    InputMethodManagerShim& operator=(const InputMethodManagerShim&) = delete;

    struct Impl;
    Impl& impl() const;
    explicit InputMethodManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<InputMethodManagerShim> input_method_manager_new(const DisplayShim& display, const SeatShim& seat, const TextInputManagerShim& tim, rust::String& error);

class DesktopGlobalsShim {
public:
    ~DesktopGlobalsShim();
    DesktopGlobalsShim(const DesktopGlobalsShim&) = delete;
    DesktopGlobalsShim& operator=(const DesktopGlobalsShim&) = delete;

    struct Impl;
    Impl& impl() const;
    explicit DesktopGlobalsShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<DesktopGlobalsShim> desktop_globals_new(const DisplayShim& display, const VulkanRendererShim& renderer, rust::Str seat_name, rust::String& error);
std::unique_ptr<DesktopGlobalsShim> desktop_globals_new_headless(const DisplayShim& display, rust::Str seat_name, rust::String& error);

class PopupTreeShim {
public:
    ~PopupTreeShim();
    PopupTreeShim(const PopupTreeShim&) = delete;
    PopupTreeShim& operator=(const PopupTreeShim&) = delete;

    bool contains(std::uint64_t surface_id) const;
    bool dismiss_grabs_outside(std::uint64_t surface_id);

    struct Impl;
    Impl& impl() const;
    explicit PopupTreeShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<PopupTreeShim> popup_tree_new();

} // namespace lum
