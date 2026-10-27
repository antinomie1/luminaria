// luminaria_rs_backend.h — C++ shims for luminaria.backend.
#pragma once

#include <cstdint>
#include <memory>

#include "luminaria/luminaria_rs.h"

namespace luminaria {
class Session;
}

namespace lum {

class SessionShim;
luminaria::Session* raw_ptr(const SessionShim& s);

// ============================================================================
// Backends: Session, DRM, Libinput, Headless, Wayland
// ============================================================================

class SessionShim {
public:
    ~SessionShim();
    SessionShim(const SessionShim&) = delete;
    SessionShim& operator=(const SessionShim&) = delete;

    bool switch_to(int vt, rust::String& error) const;
    bool switch_vt_for(std::uint32_t keysym) const;
    bool active() const;
    rust::String seat_name() const;

    struct Impl;
    Impl& impl() const;
    explicit SessionShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<SessionShim> session_new(const DisplayShim& display, rust::String& error);
std::unique_ptr<SessionShim> session_shim_from_raw(luminaria::Session* s);

class DrmBackendShim {
public:
    ~DrmBackendShim();
    DrmBackendShim(const DrmBackendShim&) = delete;
    DrmBackendShim& operator=(const DrmBackendShim&) = delete;

    bool start(rust::String& error);
    int fd() const;
    void scan_connectors();

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
    rust::String keymap() const;
    void follow_seat(const SeatShim& seat, bool touch) const;
    bool set_keymap(rust::Str xkb_text);
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
    std::uint32_t capabilities() const;
    void follow_seat(const SeatShim& seat, bool touch) const;

    struct Impl;
    Impl& impl() const;
    explicit WaylandBackendShim(std::unique_ptr<Impl> impl) noexcept;
private:
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<WaylandBackendShim> wayland_backend_new(const DisplayShim& display, rust::String& error);

} // namespace lum
