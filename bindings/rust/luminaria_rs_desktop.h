// luminaria_rs_desktop.h — plain header for the luminaria.desktop Rust binding
// bridge.
#pragma once

#include <cstdint>
#include <memory>

#include "luminaria/luminaria_rs.h"

namespace lum {

// Shared structs from desktop.rs.h
struct LockSurfaceInfoRaw;

class ForeignToplevelManagerShim {
public:
  ~ForeignToplevelManagerShim();
  ForeignToplevelManagerShim(const ForeignToplevelManagerShim &) = delete;
  ForeignToplevelManagerShim &
  operator=(const ForeignToplevelManagerShim &) = delete;

  void track(const XdgShellShim &shell);

  struct Impl;
  Impl &impl() const;
  explicit ForeignToplevelManagerShim(std::unique_ptr<Impl> impl) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};
std::unique_ptr<ForeignToplevelManagerShim>
foreign_toplevel_manager_new(const DisplayShim &display, rust::String &error);
void foreign_toplevel_track(ForeignToplevelManagerShim &ftm,
                            const XdgShellShim &shell);

class ForeignToplevelListShim {
public:
  ~ForeignToplevelListShim();
  ForeignToplevelListShim(const ForeignToplevelListShim &) = delete;
  ForeignToplevelListShim &operator=(const ForeignToplevelListShim &) = delete;

  void track(const XdgShellShim &shell);
  std::uint32_t toplevel_count() const;

  struct Impl;
  Impl &impl() const;
  explicit ForeignToplevelListShim(std::unique_ptr<Impl> impl) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};
std::unique_ptr<ForeignToplevelListShim>
foreign_toplevel_list_new(const DisplayShim &display, rust::String &error);
void foreign_toplevel_list_track(ForeignToplevelListShim &list,
                                 const XdgShellShim &shell);
std::size_t foreign_toplevel_list_resolver();

class WorkspaceManagerShim {
public:
  ~WorkspaceManagerShim();
  WorkspaceManagerShim(const WorkspaceManagerShim &) = delete;
  WorkspaceManagerShim &operator=(const WorkspaceManagerShim &) = delete;

  std::uint32_t create_group() const;
  std::uint32_t create_workspace(std::uint32_t group, rust::Str name) const;
  void remove_workspace(std::uint32_t workspace) const;
  void activate_workspace(std::uint32_t workspace) const;

  struct Impl;
  Impl &impl() const;
  explicit WorkspaceManagerShim(std::unique_ptr<Impl> impl) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};
std::unique_ptr<WorkspaceManagerShim>
workspace_manager_new(const DisplayShim &display, rust::String &error);

class SessionLockManagerShim {
public:
  ~SessionLockManagerShim();
  SessionLockManagerShim(const SessionLockManagerShim &) = delete;
  SessionLockManagerShim &operator=(const SessionLockManagerShim &) = delete;

  bool locked() const;
  std::uint64_t current() const;
  bool lock_locked(std::uint64_t lock) const;
  void send_locked(std::uint64_t lock) const;
  void send_finished(std::uint64_t lock) const;
  rust::Vec<std::uint64_t> lock_surfaces(std::uint64_t lock) const;
  bool lock_surface_info(std::uint64_t surface_id,
                         LockSurfaceInfoRaw &out) const;
  std::uint32_t lock_surface_configure(std::uint64_t surface_id,
                                       std::uint32_t width,
                                       std::uint32_t height) const;
  bool lock_surface_on_output(std::uint64_t surface_id,
                              const OutputGlobalShim &output) const;

  struct Impl;
  Impl &impl() const;
  explicit SessionLockManagerShim(std::unique_ptr<Impl> impl) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};
std::unique_ptr<SessionLockManagerShim>
session_lock_manager_new(const DisplayShim &display, rust::String &error);

class OutputManagerShim {
public:
  ~OutputManagerShim();
  OutputManagerShim(const OutputManagerShim &) = delete;
  OutputManagerShim &operator=(const OutputManagerShim &) = delete;

  void update();
  void set_auto_verdict(bool succeed) const;

  struct Impl;
  Impl &impl() const;
  explicit OutputManagerShim(std::unique_ptr<Impl> impl) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};

std::unique_ptr<OutputManagerShim>
output_manager_new(const DisplayShim &display, const OutputLayoutShim &layout,
                   rust::String &error);

class DataControlManagerShim {
public:
  ~DataControlManagerShim();
  DataControlManagerShim(const DataControlManagerShim &) = delete;
  DataControlManagerShim &operator=(const DataControlManagerShim &) = delete;

  struct Impl;
  Impl &impl() const;
  explicit DataControlManagerShim(std::unique_ptr<Impl> impl) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};

std::unique_ptr<DataControlManagerShim>
data_control_manager_new(const DisplayShim &display,
                         const DataDeviceManagerShim &ddm, rust::String &error);

class InputMethodManagerShim {
public:
  ~InputMethodManagerShim();
  InputMethodManagerShim(const InputMethodManagerShim &) = delete;
  InputMethodManagerShim &operator=(const InputMethodManagerShim &) = delete;

  std::uint64_t current() const;
  bool active(std::uint64_t input_method) const;
  bool has_keyboard_grab(std::uint64_t input_method) const;
  void grab_send_key(std::uint64_t input_method, std::uint32_t time,
                     std::uint32_t key, bool pressed) const;
  void grab_send_modifiers(std::uint64_t input_method, std::uint32_t depressed,
                           std::uint32_t latched, std::uint32_t locked,
                           std::uint32_t group) const;
  void grab_send_repeat_info(std::uint64_t input_method, std::int32_t rate,
                             std::int32_t delay) const;
  rust::Vec<std::uint64_t> popup_surfaces(std::uint64_t input_method) const;
  Rect popup_text_input_rectangle(std::uint64_t surface_id) const;

  struct Impl;
  Impl &impl() const;
  explicit InputMethodManagerShim(std::unique_ptr<Impl> impl) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};

std::unique_ptr<InputMethodManagerShim>
input_method_manager_new(const DisplayShim &display, const SeatShim &seat,
                         const TextInputManagerShim &tim, rust::String &error);

class GammaControlManagerShim {
public:
  ~GammaControlManagerShim();
  GammaControlManagerShim(const GammaControlManagerShim &) = delete;
  GammaControlManagerShim &operator=(const GammaControlManagerShim &) = delete;

  std::uint32_t gamma_size() const;
  bool get_ramp(std::uint64_t output_res_id, rust::Vec<std::uint16_t> &red,
                rust::Vec<std::uint16_t> &green,
                rust::Vec<std::uint16_t> &blue) const;

  struct Impl;
  Impl &impl() const;
  explicit GammaControlManagerShim(std::unique_ptr<Impl> impl) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};
std::unique_ptr<GammaControlManagerShim>
gamma_control_manager_new(const DisplayShim &display, std::uint32_t gamma_size,
                          rust::String &error);

class CtmControlManagerShim {
public:
  ~CtmControlManagerShim();
  CtmControlManagerShim(const CtmControlManagerShim &) = delete;
  CtmControlManagerShim &operator=(const CtmControlManagerShim &) = delete;

  bool get_ctm(std::uint64_t output_res_id,
               rust::Vec<double> &out_matrix_9) const;

  struct Impl;
  Impl &impl() const;
  explicit CtmControlManagerShim(std::unique_ptr<Impl> impl) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};
std::unique_ptr<CtmControlManagerShim>
ctm_control_manager_new(const DisplayShim &display, rust::String &error);

class VirtualKeyboardManagerShim {
public:
  ~VirtualKeyboardManagerShim();
  VirtualKeyboardManagerShim(const VirtualKeyboardManagerShim &) = delete;
  VirtualKeyboardManagerShim &
  operator=(const VirtualKeyboardManagerShim &) = delete;

  struct Impl;
  Impl &impl() const;
  explicit VirtualKeyboardManagerShim(std::unique_ptr<Impl> impl) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};
std::unique_ptr<VirtualKeyboardManagerShim>
virtual_keyboard_manager_new(const DisplayShim &display, rust::String &error);

class VirtualPointerManagerShim {
public:
  ~VirtualPointerManagerShim();
  VirtualPointerManagerShim(const VirtualPointerManagerShim &) = delete;
  VirtualPointerManagerShim &
  operator=(const VirtualPointerManagerShim &) = delete;

  struct Impl;
  Impl &impl() const;
  explicit VirtualPointerManagerShim(std::unique_ptr<Impl> impl) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};
std::unique_ptr<VirtualPointerManagerShim>
virtual_pointer_manager_new(const DisplayShim &display, rust::String &error);

} // namespace lum
