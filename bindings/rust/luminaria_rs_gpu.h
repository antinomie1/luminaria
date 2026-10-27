// luminaria_rs_gpu.h — plain header for the luminaria.gpu Rust binding bridge.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "luminaria/luminaria_rs.h"

namespace lum {

// Shared structs from gpu.rs.h
struct InputEventRaw;
struct DrmDeviceId;
struct VulkanHandlesRaw;

class SceneRendererShim;

// ============================================================================
// Bare-metal Backends (Session, DRM, Libinput)
// ============================================================================

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
    bool next_input_event(InputEventRaw& out);
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

// The backend luminaria picks (lm::AutoBackend), with its outputs, input and
// close requests queued for Rust the way the single backends queue theirs.
class AutoBackendShim {
public:
    ~AutoBackendShim();
    AutoBackendShim(const AutoBackendShim&) = delete;
    AutoBackendShim& operator=(const AutoBackendShim&) = delete;

    // 1 = nested, 2 = drm, 3 = headless.
    std::uint32_t kind() const;
    bool start(rust::String& error);
    // As DrmBackendShim::next_event; nested and headless report theirs at start.
    std::unique_ptr<OutputShim> next_event(std::uint32_t& kind, std::uint64_t& removed);
    bool next_input_event(InputEventRaw& out);
    rust::String keymap() const;
    std::uint32_t capabilities() const;
    bool take_close_request();
    bool has_session() const;
    // Only when has_session(); borrowed from the backend, which owns it.
    const SessionShim& session() const;
    rust::String session_error() const;
    bool drm_device(DrmDeviceId& out) const;

    struct Impl;
    Impl& impl() const;
    explicit AutoBackendShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

// kind: 0 = auto, 1 = nested, 2 = drm, 3 = headless. An empty device: the first card with a monitor.
std::unique_ptr<AutoBackendShim> auto_backend_new(const DisplayShim& display, std::uint32_t kind, rust::Str drm_device,
                                                  std::int32_t width, std::int32_t height, rust::Str title,
                                                  rust::String& error);

// ============================================================================
// GPU Protocols & Rendering
// ============================================================================

class DrmSyncobjManagerShim {
public:
    ~DrmSyncobjManagerShim();
    DrmSyncobjManagerShim(const DrmSyncobjManagerShim&) = delete;
    DrmSyncobjManagerShim& operator=(const DrmSyncobjManagerShim&) = delete;
    struct Impl;
    Impl& impl() const;
    explicit DrmSyncobjManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<DrmSyncobjManagerShim> drm_syncobj_manager_new(const DisplayShim& display, rust::String& error);

class ScreencopyManagerShim {
public:
    ~ScreencopyManagerShim();
    ScreencopyManagerShim(const ScreencopyManagerShim&) = delete;
    ScreencopyManagerShim& operator=(const ScreencopyManagerShim&) = delete;
    struct Impl;
    Impl& impl() const;
    explicit ScreencopyManagerShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<ScreencopyManagerShim> screencopy_manager_new(const DisplayShim& display, rust::String& error);

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
std::unique_ptr<LinuxDmabufShim> linux_dmabuf_new_from_scene_renderer(const DisplayShim& display, const SceneRendererShim& renderer, rust::String& error);

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
std::unique_ptr<CursorShim> cursor_new_with_shapes(const SceneRendererShim& renderer, const OutputLayoutShim& layout,
                                                  const SeatShim& seat, const CursorShapeManagerShim& shapes,
                                                  rust::String& error);

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

} // namespace lum
