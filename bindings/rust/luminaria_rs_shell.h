// luminaria_rs_shell.h — C++ shims for luminaria.shell.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "luminaria/luminaria_rs.h"
#include "luminaria/luminaria_rs_backend.h"
#include "luminaria/luminaria_rs_render.h"

namespace lum {

struct SceneItemRaw;
struct SceneHitRaw;
struct ScreenOptionsRaw;

SceneHitRaw raw_scene_hit_test(rust::Slice<const SceneItemRaw> items, double x, double y);
SceneHitRaw raw_scene_route_pointer(const SeatShim& seat, rust::Slice<const SceneItemRaw> items, double x, double y);
bool raw_scene_surface_origin(rust::Slice<const SceneItemRaw> items, std::uint64_t surface_id, double& x, double& y);

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

class AutoBackendShim {
public:
    ~AutoBackendShim();
    AutoBackendShim(const AutoBackendShim&) = delete;
    AutoBackendShim& operator=(const AutoBackendShim&) = delete;

    std::uint32_t kind() const;
    bool start(rust::String& error) const;
    rust::String keymap() const;
    bool set_keymap(rust::Str xkb_text) const;
    std::uint32_t capabilities() const;
    void follow_seat(const SeatShim& seat, bool touch) const;
    bool has_session() const;
    const SessionShim& session() const;
    rust::String session_error() const;
    bool drm_device(DrmDeviceId& out) const;

    struct Impl;
    Impl& impl() const;
    explicit AutoBackendShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<AutoBackendShim> auto_backend_new(const DisplayShim& display, std::uint32_t kind, rust::Str drm_device,
                                                  std::int32_t width, std::int32_t height, rust::Str title,
                                                  rust::String& error);

class SceneRendererShim {
public:
    ~SceneRendererShim();
    SceneRendererShim(const SceneRendererShim&) = delete;
    SceneRendererShim& operator=(const SceneRendererShim&) = delete;

    bool gpu_enabled() const;
    bool has_retained(std::uint64_t surface_id) const;
    rust::String gpu_error() const;
    bool reset(OutputShim& output, std::uint32_t drm_format, rust::String& error) const;
    int present(OutputShim& output, rust::Slice<const SceneItemRaw> items,
                float bg_r, float bg_g, float bg_b, float bg_a, rust::String& error) const;
    int present_in(const OutputLayoutShim& layout, OutputShim& output, rust::Slice<const SceneItemRaw> items,
                   float bg_r, float bg_g, float bg_b, float bg_a, rust::String& error) const;
    bool gpu_output(const OutputShim& output) const;
    void set_presentation(const PresentationShim& presentation) const;
    void clear_presentation() const;
    void update_cursor() const;
    rust::Vec<std::uint8_t> read_back(const OutputShim& output, rust::String& error) const;
    bool vulkan_handles(VulkanHandlesRaw& out) const;
    bool set_image(std::uint64_t serial, std::int32_t width, std::int32_t height, rust::Slice<const std::uint8_t> rgba, rust::String& error) const;
    void remove_image(std::uint64_t serial) const;
    void clear_images() const;
    bool has_image(std::uint64_t serial) const;

    struct Impl;
    Impl& impl() const;
    explicit SceneRendererShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<SceneRendererShim> scene_renderer_new(bool has_drm_device, const DrmDeviceId& drm_device, rust::String& error);

std::unique_ptr<LinuxDmabufShim> linux_dmabuf_new_from_scene_renderer(const DisplayShim& display, const SceneRendererShim& renderer, rust::String& error);

class CursorShim {
public:
    ~CursorShim();
    CursorShim(const CursorShim&) = delete;
    CursorShim& operator=(const CursorShim&) = delete;

    bool move_to(double x, double y);
    bool move_by(double dx, double dy);
    bool move_to_within(double x, double y, Rect bounds);
    bool move_by_within(double dx, double dy, Rect bounds);
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
std::unique_ptr<CursorShim> cursor_new_with_shapes(const SceneRendererShim& renderer, const OutputLayoutShim& layout,
                                                  const SeatShim& seat, const CursorShapeManagerShim& shapes,
                                                  rust::String& error);
std::unique_ptr<CursorShim> cursor_new_with_capture(const SceneRendererShim& renderer, const OutputLayoutShim& layout,
                                                    const SeatShim& seat, const CursorShapeManagerShim& shapes,
                                                    const ScreencopyManagerShim& capture, rust::String& error);
void screencopy_use_scene_renderer(ScreencopyManagerShim& mgr, const SceneRendererShim& renderer);

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

class PopupTreeShim {
public:
    ~PopupTreeShim();
    PopupTreeShim(const PopupTreeShim&) = delete;
    PopupTreeShim& operator=(const PopupTreeShim&) = delete;

    bool set_parent(const XdgShellShim& shell, std::uint64_t popup, std::uint64_t parent) const;
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
    ~LayerManagerShim();
    LayerManagerShim(const LayerManagerShim&) = delete;
    LayerManagerShim& operator=(const LayerManagerShim&) = delete;

    bool add(const LayerShellShim& shell, std::uint64_t surface_id, std::uint32_t output_id);
    void remove(std::uint64_t surface_id);
    void set_mapped(std::uint64_t surface_id, bool mapped);
    Rect arrange(std::uint32_t output_id, Rect full, Rect usable);
    rust::Vec<SceneItemRaw> append_to(std::uint32_t output_id, std::uint32_t layer) const;
    std::uint64_t keyboard_target(std::uint64_t surface_id) const;
    void close_output(std::uint32_t output_id);
    rust::Vec<std::uint64_t> surfaces_on(std::uint32_t output_id) const;

    struct Impl;
    Impl& impl() const;
    explicit LayerManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<LayerManagerShim> layer_manager_new();

// C++ never calls Rust, so the scene a builder would return is told ahead of
// time: `set_scene()` replaces the list, and everything that asks for the scene
// inside dispatch — a frame, a click, the shell placing a popup — reads that.
class ScreenManagerShim {
public:
    ~ScreenManagerShim();
    ScreenManagerShim(const ScreenManagerShim&) = delete;
    ScreenManagerShim& operator=(const ScreenManagerShim&) = delete;

    void follow_popups(const PopupTreeShim& popups) const;
    void follow_capture(const ScreencopyManagerShim& capture) const;
    void follow_cursor(const CursorShim& cursor) const;
    void set_scene(rust::Slice<const SceneItemRaw> items) const;
    void set_background(float r, float g, float b, float a) const;
    std::uint64_t output_identity(const OutputShim& output) const;
    std::uint64_t output_global(const OutputShim& output) const;
    rust::String output_name(const OutputShim& output) const;
    bool output_is(const LayerShellShim& shell, std::uint64_t surface_id, const OutputShim& output) const;
    bool surface_box(std::uint64_t surface_id, Rect& out) const;
    void invalidate() const;
    std::size_t size() const;

    struct Impl;
    Impl& impl() const;
    explicit ScreenManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<ScreenManagerShim> screen_manager_new(const DisplayShim& display, const AutoBackendShim& backend,
                                                      const SceneRendererShim& renderer, const OutputLayoutShim& layout,
                                                      const ScreenOptionsRaw& options, rust::String& error);

// For the same reason a binding is a table made ahead of time, and what a key or
// a button means arrives afterwards, as an event.
class InputRouterShim {
public:
    ~InputRouterShim();
    InputRouterShim(const InputRouterShim&) = delete;
    InputRouterShim& operator=(const InputRouterShim&) = delete;

    void attach(const AutoBackendShim& backend) const;
    void bind(std::uint32_t mods, std::uint32_t keysym, std::uint32_t id) const;
    void set_click_to_focus(bool enabled) const;
    void update_pointer() const;
    void route_key(std::uint32_t key, bool pressed, std::uint32_t keysym, std::uint32_t depressed,
                   std::uint32_t latched, std::uint32_t locked, std::uint32_t group) const;
    void route_modifiers(std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked,
                         std::uint32_t group) const;
    void route_motion(double dx, double dy) const;
    void route_motion_absolute(double x, double y) const;
    void route_leave() const;
    void route_button(std::uint32_t button, bool pressed) const;
    void route_axis(double dx, double dy, std::int32_t dx_steps, std::int32_t dy_steps,
                    bool stop_horizontal, bool stop_vertical) const;

    struct Impl;
    Impl& impl() const;
    explicit InputRouterShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<InputRouterShim> input_router_new(const SeatShim& seat, const CursorShim& cursor,
                                                  const ScreenManagerShim& screens, const XdgShellShim& shell,
                                                  const PopupTreeShim& popups, rust::String& error);

} // namespace lum
