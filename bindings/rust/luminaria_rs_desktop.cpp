// luminaria_rs_desktop.cpp — C++ shim for luminaria.desktop.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "luminaria/luminaria_rs.h"
#include "luminaria/luminaria_rs_desktop.h"
#include "luminaria/src/ffi.rs.h"
#include "luminaria/src/ffi_desktop.rs.h"

import luminaria.desktop;

#include "luminaria_rs_internal.h"

namespace lum {

// ============================================================================
// Desktop Protocols
// ============================================================================

struct ForeignToplevelManagerShim::Impl {
  std::optional<lm::ForeignToplevelManager> mgr;
  Events events;
  Conn<lm::ForeignToplevelRequest> on_request;
};
ForeignToplevelManagerShim::~ForeignToplevelManagerShim() = default;
ForeignToplevelManagerShim::ForeignToplevelManagerShim(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
ForeignToplevelManagerShim::Impl &ForeignToplevelManagerShim::impl() const {
  return *impl_;
}

std::unique_ptr<ForeignToplevelManagerShim>
foreign_toplevel_manager_new(const DisplayShim &display, rust::String &error) {
  try {
    lm::Display *disp = raw_ptr(display);
    if (!disp) {
      error = "display not initialized";
      return nullptr;
    }
    auto r = lm::ForeignToplevelManager::create(*disp);
    if (!r) {
      error = to_rust(r.error().message);
      return nullptr;
    }
    auto impl = std::make_unique<ForeignToplevelManagerShim::Impl>();
    impl->mgr.emplace(std::move(*r));
    auto *raw = impl.get();
    raw->on_request =
        raw->mgr->request().connect([raw](lm::ForeignToplevelRequest &e) {
          EventRaw ev = event(EventKind::ForeignToplevelRequest,
                              surface_u64(&e.toplevel.surface()));
          ev.value = static_cast<std::uint32_t>(e.kind);
          raw->events.push(std::move(ev));
        });
    return std::make_unique<ForeignToplevelManagerShim>(std::move(impl));
  } catch (const std::exception &e) {
    error = e.what();
    return nullptr;
  }
}

void ForeignToplevelManagerShim::track(const XdgShellShim &shell) {
  lm::XdgShell *sh = raw_ptr(shell);
  if (impl_->mgr && sh) {
    impl_->mgr->track(*sh);
  }
}

void foreign_toplevel_track(ForeignToplevelManagerShim &ftm,
                            const XdgShellShim &shell) {
  ftm.track(shell);
}

struct WorkspaceManagerShim::Impl {
  std::optional<lm::WorkspaceManager> mgr;
  Events events;
  Conn<lm::WorkspaceRequest> on_request;
};
WorkspaceManagerShim::~WorkspaceManagerShim() = default;
WorkspaceManagerShim::WorkspaceManagerShim(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
WorkspaceManagerShim::Impl &WorkspaceManagerShim::impl() const {
  return *impl_;
}

std::unique_ptr<WorkspaceManagerShim>
workspace_manager_new(const DisplayShim &display, rust::String &error) {
  try {
    lm::Display *disp = raw_ptr(display);
    if (!disp) {
      error = "display not initialized";
      return nullptr;
    }
    auto r = lm::WorkspaceManager::create(*disp);
    if (!r) {
      error = to_rust(r.error().message);
      return nullptr;
    }
    auto impl = std::make_unique<WorkspaceManagerShim::Impl>();
    impl->mgr.emplace(std::move(*r));
    auto *raw = impl.get();
    raw->on_request =
        raw->mgr->request().connect([raw](lm::WorkspaceRequest &e) {
          EventRaw ev =
              event(EventKind::WorkspaceRequest, e.workspace, e.group);
          ev.value = static_cast<std::uint32_t>(e.kind);
          ev.text = to_rust(e.name);
          raw->events.push(std::move(ev));
        });
    return std::make_unique<WorkspaceManagerShim>(std::move(impl));
  } catch (const std::exception &e) {
    error = e.what();
    return nullptr;
  }
}

std::uint32_t WorkspaceManagerShim::create_group() const {
  return impl_->mgr ? impl_->mgr->add_group() : 0;
}
std::uint32_t WorkspaceManagerShim::create_workspace(std::uint32_t group,
                                                     rust::Str name) const {
  return impl_->mgr ? impl_->mgr->add_workspace(group, std::string(name),
                                                std::string(name))
                    : 0;
}
void WorkspaceManagerShim::remove_workspace(std::uint32_t workspace) const {
  if (impl_->mgr)
    impl_->mgr->remove_workspace(workspace);
}
void WorkspaceManagerShim::activate_workspace(std::uint32_t workspace) const {
  if (impl_->mgr)
    impl_->mgr->activate(workspace);
}

struct SessionLockManagerShim::Impl {
  struct LockEntry {
    lm::SessionLock *lock = nullptr;
    Conn<lm::NewSessionLockSurface> on_surface;
    Conn<lm::SessionLockDestroy> on_destroy;
  };
  struct SurfaceEntry {
    lm::SessionLockSurface *surface = nullptr;
    std::uint64_t lock = 0;
    Conn<lm::SessionLockSurfaceMap> on_map;
    Conn<lm::SessionLockSurfaceDestroy> on_destroy;
  };

  std::optional<lm::SessionLockManager> mgr;
  std::unordered_map<std::uint64_t, std::unique_ptr<LockEntry>> locks;
  std::unordered_map<std::uint64_t, std::unique_ptr<SurfaceEntry>> surfaces;
  Events events;
  Conn<lm::NewSessionLock> on_new_lock;

  lm::SessionLock *lock(std::uint64_t id) const {
    const auto it = locks.find(id);
    return it == locks.end() ? nullptr : it->second->lock;
  }
  const SurfaceEntry *surface(std::uint64_t id) const {
    const auto it = surfaces.find(id);
    return it == surfaces.end() ? nullptr : it->second.get();
  }

  void watch(lm::SessionLockSurface &ls, std::uint64_t lock_id) {
    const std::uint64_t id = u64_from_id(ls.surface().id());
    auto entry = std::make_unique<SurfaceEntry>();
    entry->surface = &ls;
    entry->lock = lock_id;
    entry->on_map = ls.map.connect([this, id](lm::SessionLockSurfaceMap &) {
      events.push(event(EventKind::LockSurfaceMap, id));
    });
    entry->on_destroy =
        ls.destroy.connect([this, id](lm::SessionLockSurfaceDestroy &) {
          events.push(event(EventKind::LockSurfaceDestroy, id));
          surfaces.erase(id);
        });
    events.push(event(EventKind::NewLockSurface, lock_id, id));
    surfaces[id] = std::move(entry);
  }

  void watch(lm::SessionLock &lock) {
    const std::uint64_t id = next_object_id();
    auto entry = std::make_unique<LockEntry>();
    entry->lock = &lock;
    entry->on_surface =
        lock.new_surface.connect([this, id](lm::NewSessionLockSurface &e) {
          watch(e.lock_surface, id);
        });
    entry->on_destroy =
        lock.destroy.connect([this, id](lm::SessionLockDestroy &e) {
          EventRaw ev = event(EventKind::SessionLockDestroy, id);
          ev.flag = e.unlocked;
          events.push(std::move(ev));
          locks.erase(id);
        });
    events.push(event(EventKind::NewSessionLock, id));
    locks[id] = std::move(entry);
  }
};
SessionLockManagerShim::~SessionLockManagerShim() = default;
SessionLockManagerShim::SessionLockManagerShim(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
SessionLockManagerShim::Impl &SessionLockManagerShim::impl() const {
  return *impl_;
}
std::unique_ptr<SessionLockManagerShim>
session_lock_manager_new(const DisplayShim &display, rust::String &error) {
  try {
    lm::Display *disp = raw_ptr(display);
    if (!disp) {
      error = "display not initialized";
      return nullptr;
    }
    auto r = lm::SessionLockManager::create(*disp);
    if (!r) {
      error = to_rust(r.error().message);
      return nullptr;
    }
    auto impl = std::make_unique<SessionLockManagerShim::Impl>();
    impl->mgr.emplace(std::move(*r));
    auto *raw = impl.get();
    raw->on_new_lock = raw->mgr->new_lock().connect(
        [raw](lm::NewSessionLock &e) { raw->watch(e.lock); });
    return std::make_unique<SessionLockManagerShim>(std::move(impl));
  } catch (const std::exception &e) {
    error = e.what();
    return nullptr;
  }
}
bool SessionLockManagerShim::locked() const {
  return impl_->mgr && impl_->mgr->session_locked();
}
std::uint64_t SessionLockManagerShim::current() const {
  if (!impl_->mgr)
    return 0;
  const lm::SessionLock *current = impl_->mgr->current();
  for (const auto &[id, entry] : impl_->locks) {
    if (current != nullptr && entry->lock == current)
      return id;
  }
  return 0;
}
bool SessionLockManagerShim::lock_locked(std::uint64_t lock) const {
  const lm::SessionLock *l = impl_->lock(lock);
  return l != nullptr && l->locked();
}
void SessionLockManagerShim::send_locked(std::uint64_t lock) const {
  if (lm::SessionLock *l = impl_->lock(lock); l != nullptr)
    l->send_locked();
}
void SessionLockManagerShim::send_finished(std::uint64_t lock) const {
  if (lm::SessionLock *l = impl_->lock(lock); l != nullptr)
    l->send_finished();
}
rust::Vec<std::uint64_t>
SessionLockManagerShim::lock_surfaces(std::uint64_t lock) const {
  rust::Vec<std::uint64_t> out;
  if (const lm::SessionLock *l = impl_->lock(lock); l != nullptr) {
    for (lm::SessionLockSurface *s : l->surfaces())
      out.push_back(u64_from_id(s->surface().id()));
  }
  return out;
}
bool SessionLockManagerShim::lock_surface_info(std::uint64_t surface_id,
                                               LockSurfaceInfoRaw &out) const {
  const Impl::SurfaceEntry *e = impl_->surface(surface_id);
  if (e == nullptr)
    return false;
  out.lock = e->lock;
  out.mapped = e->surface->mapped();
  out.width = e->surface->width();
  out.height = e->surface->height();
  out.has_output = e->surface->output_resource() != nullptr;
  return true;
}
std::uint32_t SessionLockManagerShim::lock_surface_configure(
    std::uint64_t surface_id, std::uint32_t width, std::uint32_t height) const {
  const Impl::SurfaceEntry *e = impl_->surface(surface_id);
  return e != nullptr ? e->surface->configure(width, height) : 0;
}
bool SessionLockManagerShim::lock_surface_on_output(
    std::uint64_t surface_id, const OutputGlobalShim &output) const {
  const Impl::SurfaceEntry *e = impl_->surface(surface_id);
  return e != nullptr && is_output(e->surface->output_resource(), output);
}

struct OutputManagerShim::Impl {
  std::optional<lm::OutputManager> mgr;
  Events events;
  Conn<lm::OutputConfigRequest> on_request;
  lm::OutputConfigVerdict auto_verdict = lm::OutputConfigVerdict::succeeded;
};
OutputManagerShim::~OutputManagerShim() = default;
OutputManagerShim::OutputManagerShim(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
OutputManagerShim::Impl &OutputManagerShim::impl() const { return *impl_; }

std::unique_ptr<OutputManagerShim>
output_manager_new(const DisplayShim &display, const OutputLayoutShim &layout,
                   rust::String &error) {
  try {
    lm::Display *disp = raw_ptr(display);
    if (!disp) {
      error = "display not initialized";
      return nullptr;
    }
    auto r = lm::OutputManager::create(*disp, raw_ref(layout));
    if (!r) {
      error = to_rust(r.error().message);
      return nullptr;
    }
    auto impl = std::make_unique<OutputManagerShim::Impl>();
    impl->mgr.emplace(std::move(*r));
    auto *raw = impl.get();
    raw->on_request =
        raw->mgr->request().connect([raw](lm::OutputConfigRequest &r) {
          r.verdict = raw->auto_verdict;
          EventRaw ev = event(EventKind::OutputConfigRequest, 0);
          ev.flag = r.test;
          raw->events.push(std::move(ev));
        });
    return std::make_unique<OutputManagerShim>(std::move(impl));
  } catch (const std::exception &e) {
    error = e.what();
    return nullptr;
  }
}
void OutputManagerShim::update() {
  if (impl_->mgr)
    impl_->mgr->update();
}
void OutputManagerShim::set_auto_verdict(bool succeed) const {
  impl_->auto_verdict = succeed ? lm::OutputConfigVerdict::succeeded
                                : lm::OutputConfigVerdict::failed;
}

struct DataControlManagerShim::Impl {
  std::optional<lm::DataControlManager> mgr;
};
DataControlManagerShim::~DataControlManagerShim() = default;
DataControlManagerShim::DataControlManagerShim(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
DataControlManagerShim::Impl &DataControlManagerShim::impl() const {
  return *impl_;
}

std::unique_ptr<DataControlManagerShim>
data_control_manager_new(const DisplayShim &display,
                         const DataDeviceManagerShim &ddm,
                         rust::String &error) {
  try {
    lm::Display *disp = raw_ptr(display);
    lm::DataDeviceManager *ddm_ptr = raw_ptr(ddm);
    if (!disp || !ddm_ptr) {
      error = "null display or data_device";
      return nullptr;
    }
    auto r = lm::DataControlManager::create(*disp, *ddm_ptr, nullptr);
    if (!r) {
      error = to_rust(r.error().message);
      return nullptr;
    }
    auto impl = std::make_unique<DataControlManagerShim::Impl>();
    impl->mgr.emplace(std::move(*r));
    return std::make_unique<DataControlManagerShim>(std::move(impl));
  } catch (const std::exception &e) {
    error = e.what();
    return nullptr;
  }
}

struct InputMethodManagerShim::Impl {
  struct Entry {
    lm::InputMethod *input_method = nullptr;
    Conn<lm::NewInputPopupSurface> on_popup;
    Conn<lm::InputMethodGrabChange> on_grab;
    Conn<lm::InputMethodDestroy> on_destroy;
  };
  struct PopupEntry {
    lm::InputPopupSurface *popup = nullptr;
    Conn<lm::InputPopupSurfaceDestroy> on_destroy;
  };

  std::optional<lm::InputMethodManager> mgr;
  std::unordered_map<std::uint64_t, std::unique_ptr<Entry>> input_methods;
  std::unordered_map<std::uint64_t, std::unique_ptr<PopupEntry>> popups;
  Events events;
  Conn<lm::NewInputMethod> on_new;

  lm::InputMethod *find(std::uint64_t id) const {
    const auto it = input_methods.find(id);
    return it == input_methods.end() ? nullptr : it->second->input_method;
  }

  void watch(lm::InputPopupSurface &popup, std::uint64_t im) {
    const std::uint64_t id = u64_from_id(popup.surface().id());
    auto entry = std::make_unique<PopupEntry>();
    entry->popup = &popup;
    entry->on_destroy =
        popup.destroy.connect([this, id](lm::InputPopupSurfaceDestroy &) {
          events.push(event(EventKind::InputPopupSurfaceDestroy, id));
          popups.erase(id);
        });
    events.push(event(EventKind::NewInputPopupSurface, im, id));
    popups[id] = std::move(entry);
  }

  void watch(lm::InputMethod &im) {
    const std::uint64_t id = next_object_id();
    auto entry = std::make_unique<Entry>();
    entry->input_method = &im;
    entry->on_popup = im.new_popup_surface.connect(
        [this, id](lm::NewInputPopupSurface &e) { watch(e.popup, id); });
    entry->on_grab =
        im.grab_changed.connect([this, id](lm::InputMethodGrabChange &e) {
          EventRaw ev = event(EventKind::InputMethodGrabChange, id);
          ev.flag = e.grabbed;
          events.push(std::move(ev));
        });
    entry->on_destroy =
        im.destroy.connect([this, id](lm::InputMethodDestroy &) {
          events.push(event(EventKind::InputMethodDestroy, id));
          input_methods.erase(id);
        });
    events.push(event(EventKind::NewInputMethod, id));
    input_methods[id] = std::move(entry);
  }
};
InputMethodManagerShim::~InputMethodManagerShim() = default;
InputMethodManagerShim::InputMethodManagerShim(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
InputMethodManagerShim::Impl &InputMethodManagerShim::impl() const {
  return *impl_;
}

std::unique_ptr<InputMethodManagerShim>
input_method_manager_new(const DisplayShim &display, const SeatShim &seat,
                         const TextInputManagerShim &tim, rust::String &error) {
  try {
    lm::Display *disp = raw_ptr(display);
    lm::Seat *seat_ptr = raw_ptr(seat);
    lm::TextInputManager *tim_ptr = raw_ptr(tim);
    if (!disp || !seat_ptr || !tim_ptr) {
      error = "null display, seat or text_inputs";
      return nullptr;
    }
    auto r = lm::InputMethodManager::create(*disp, *seat_ptr, *tim_ptr);
    if (!r) {
      error = to_rust(r.error().message);
      return nullptr;
    }
    auto impl = std::make_unique<InputMethodManagerShim::Impl>();
    impl->mgr.emplace(std::move(*r));
    auto *raw = impl.get();
    raw->on_new = raw->mgr->new_input_method().connect(
        [raw](lm::NewInputMethod &e) { raw->watch(e.input_method); });
    return std::make_unique<InputMethodManagerShim>(std::move(impl));
  } catch (const std::exception &e) {
    error = e.what();
    return nullptr;
  }
}
std::uint64_t InputMethodManagerShim::current() const {
  if (!impl_->mgr)
    return 0;
  const lm::InputMethod *current = impl_->mgr->current();
  for (const auto &[id, entry] : impl_->input_methods) {
    if (current != nullptr && entry->input_method == current)
      return id;
  }
  return 0;
}
bool InputMethodManagerShim::active(std::uint64_t input_method) const {
  const lm::InputMethod *im = impl_->find(input_method);
  return im != nullptr && im->active();
}
bool InputMethodManagerShim::has_keyboard_grab(
    std::uint64_t input_method) const {
  lm::InputMethod *im = impl_->find(input_method);
  return im != nullptr && im->keyboard_grab() != nullptr;
}
void InputMethodManagerShim::grab_send_key(std::uint64_t input_method,
                                           std::uint32_t time,
                                           std::uint32_t key,
                                           bool pressed) const {
  if (lm::InputMethod *im = impl_->find(input_method);
      im != nullptr && im->keyboard_grab() != nullptr) {
    im->keyboard_grab()->send_key(time, key, pressed);
  }
}
void InputMethodManagerShim::grab_send_modifiers(std::uint64_t input_method,
                                                 std::uint32_t depressed,
                                                 std::uint32_t latched,
                                                 std::uint32_t locked,
                                                 std::uint32_t group) const {
  if (lm::InputMethod *im = impl_->find(input_method);
      im != nullptr && im->keyboard_grab() != nullptr) {
    im->keyboard_grab()->send_modifiers(depressed, latched, locked, group);
  }
}
void InputMethodManagerShim::grab_send_repeat_info(std::uint64_t input_method,
                                                   std::int32_t rate,
                                                   std::int32_t delay) const {
  if (lm::InputMethod *im = impl_->find(input_method);
      im != nullptr && im->keyboard_grab() != nullptr) {
    im->keyboard_grab()->send_repeat_info(rate, delay);
  }
}
rust::Vec<std::uint64_t>
InputMethodManagerShim::popup_surfaces(std::uint64_t input_method) const {
  rust::Vec<std::uint64_t> out;
  if (const lm::InputMethod *im = impl_->find(input_method); im != nullptr) {
    for (lm::InputPopupSurface *p : im->popup_surfaces())
      out.push_back(u64_from_id(p->surface().id()));
  }
  return out;
}
Rect InputMethodManagerShim::popup_text_input_rectangle(
    std::uint64_t surface_id) const {
  const auto it = impl_->popups.find(surface_id);
  return it == impl_->popups.end()
             ? Rect{0, 0, 0, 0}
             : to_rect(it->second->popup->text_input_rectangle());
}

// ============================================================================
// Gamma, CTM, Virtual Keyboard, Virtual Pointer
// ============================================================================

struct GammaControlManagerShim::Impl {
  std::optional<lm::GammaControlManager> mgr;
  std::unordered_map<std::uint64_t, lm::GammaRamp> ramps;
  Events events;
  Conn<lm::GammaRampUpdate> on_gamma;
};
GammaControlManagerShim::~GammaControlManagerShim() = default;
GammaControlManagerShim::GammaControlManagerShim(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
GammaControlManagerShim::Impl &GammaControlManagerShim::impl() const {
  return *impl_;
}

std::unique_ptr<GammaControlManagerShim>
gamma_control_manager_new(const DisplayShim &display, std::uint32_t gamma_size,
                          rust::String &error) {
  try {
    lm::Display *disp = raw_ptr(display);
    if (!disp) {
      error = "display not initialized";
      return nullptr;
    }
    auto r = lm::GammaControlManager::create(*disp, gamma_size);
    if (!r) {
      error = to_rust(r.error().message);
      return nullptr;
    }
    auto impl = std::make_unique<GammaControlManagerShim::Impl>();
    impl->mgr.emplace(std::move(*r));
    auto *raw = impl.get();
    raw->on_gamma =
        raw->mgr->gamma_changed().connect([raw](lm::GammaRampUpdate &e) {
          const std::uint64_t out_id =
              reinterpret_cast<std::uintptr_t>(e.output_resource);
          if (e.ramp.has_value()) {
            raw->ramps[out_id] = *e.ramp;
          } else {
            raw->ramps.erase(out_id);
          }
          EventRaw ev = event(EventKind::GammaChange, out_id);
          ev.flag = e.ramp.has_value();
          raw->events.push(std::move(ev));
        });
    return std::make_unique<GammaControlManagerShim>(std::move(impl));
  } catch (const std::exception &e) {
    error = e.what();
    return nullptr;
  }
}

std::uint32_t GammaControlManagerShim::gamma_size() const {
  return impl_->mgr ? impl_->mgr->gamma_size() : 0;
}

bool GammaControlManagerShim::get_ramp(std::uint64_t output_res_id,
                                       rust::Vec<std::uint16_t> &red,
                                       rust::Vec<std::uint16_t> &green,
                                       rust::Vec<std::uint16_t> &blue) const {
  const auto it = impl_->ramps.find(output_res_id);
  if (it == impl_->ramps.end())
    return false;
  for (uint16_t r : it->second.red)
    red.push_back(r);
  for (uint16_t g : it->second.green)
    green.push_back(g);
  for (uint16_t b : it->second.blue)
    blue.push_back(b);
  return true;
}

struct CtmControlManagerShim::Impl {
  std::optional<lm::CtmControlManager> mgr;
  std::unordered_map<std::uint64_t, lm::ColorTransformMatrix> matrices;
  Events events;
  Conn<lm::CtmUpdate> on_ctm;
};
CtmControlManagerShim::~CtmControlManagerShim() = default;
CtmControlManagerShim::CtmControlManagerShim(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
CtmControlManagerShim::Impl &CtmControlManagerShim::impl() const {
  return *impl_;
}

std::unique_ptr<CtmControlManagerShim>
ctm_control_manager_new(const DisplayShim &display, rust::String &error) {
  try {
    lm::Display *disp = raw_ptr(display);
    if (!disp) {
      error = "display not initialized";
      return nullptr;
    }
    auto r = lm::CtmControlManager::create(*disp);
    if (!r) {
      error = to_rust(r.error().message);
      return nullptr;
    }
    auto impl = std::make_unique<CtmControlManagerShim::Impl>();
    impl->mgr.emplace(std::move(*r));
    auto *raw = impl.get();
    raw->on_ctm = raw->mgr->ctm_changed().connect([raw](lm::CtmUpdate &e) {
      const std::uint64_t out_id =
          reinterpret_cast<std::uintptr_t>(e.output_resource);
      raw->matrices[out_id] = e.matrix;
      EventRaw ev = event(EventKind::CtmChange, out_id);
      raw->events.push(std::move(ev));
    });
    return std::make_unique<CtmControlManagerShim>(std::move(impl));
  } catch (const std::exception &e) {
    error = e.what();
    return nullptr;
  }
}

bool CtmControlManagerShim::get_ctm(std::uint64_t output_res_id,
                                    rust::Vec<double> &out_matrix_9) const {
  const auto it = impl_->matrices.find(output_res_id);
  if (it == impl_->matrices.end())
    return false;
  for (double v : it->second)
    out_matrix_9.push_back(v);
  return true;
}

struct VirtualKeyboardManagerShim::Impl {
  std::optional<lm::VirtualKeyboardManager> mgr;
  Events events;
  Conn<lm::VirtualKey> on_key;
  Conn<lm::VirtualModifiers> on_mods;
};
VirtualKeyboardManagerShim::~VirtualKeyboardManagerShim() = default;
VirtualKeyboardManagerShim::VirtualKeyboardManagerShim(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
VirtualKeyboardManagerShim::Impl &VirtualKeyboardManagerShim::impl() const {
  return *impl_;
}

std::unique_ptr<VirtualKeyboardManagerShim>
virtual_keyboard_manager_new(const DisplayShim &display, rust::String &error) {
  try {
    lm::Display *disp = raw_ptr(display);
    if (!disp) {
      error = "display not initialized";
      return nullptr;
    }
    auto r = lm::VirtualKeyboardManager::create(*disp);
    if (!r) {
      error = to_rust(r.error().message);
      return nullptr;
    }
    auto impl = std::make_unique<VirtualKeyboardManagerShim::Impl>();
    impl->mgr.emplace(std::move(*r));
    auto *raw = impl.get();
    raw->on_key = raw->mgr->key_event().connect([raw](lm::VirtualKey &e) {
      EventRaw ev = event(EventKind::VirtualKey,
                          reinterpret_cast<std::uintptr_t>(e.seat_resource));
      ev.value = e.key;
      ev.serial = e.time;
      ev.flag = e.pressed;
      raw->events.push(std::move(ev));
    });
    raw->on_mods =
        raw->mgr->modifiers_event().connect([raw](lm::VirtualModifiers &e) {
          EventRaw ev =
              event(EventKind::VirtualModifiers,
                    reinterpret_cast<std::uintptr_t>(e.seat_resource));
          ev.value = e.depressed;
          ev.serial = e.latched;
          ev.x = static_cast<std::int32_t>(e.locked);
          ev.y = static_cast<std::int32_t>(e.group);
          raw->events.push(std::move(ev));
        });
    return std::make_unique<VirtualKeyboardManagerShim>(std::move(impl));
  } catch (const std::exception &e) {
    error = e.what();
    return nullptr;
  }
}

struct VirtualPointerManagerShim::Impl {
  std::optional<lm::VirtualPointerManager> mgr;
  Events events;
  Conn<lm::VirtualPointerMotion> on_motion;
  Conn<lm::VirtualPointerMotionAbsolute> on_motion_abs;
  Conn<lm::VirtualPointerButton> on_button;
  Conn<lm::VirtualPointerAxis> on_axis;
  Conn<lm::VirtualPointerFrame> on_frame;
};
VirtualPointerManagerShim::~VirtualPointerManagerShim() = default;
VirtualPointerManagerShim::VirtualPointerManagerShim(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
VirtualPointerManagerShim::Impl &VirtualPointerManagerShim::impl() const {
  return *impl_;
}

std::unique_ptr<VirtualPointerManagerShim>
virtual_pointer_manager_new(const DisplayShim &display, rust::String &error) {
  try {
    lm::Display *disp = raw_ptr(display);
    if (!disp) {
      error = "display not initialized";
      return nullptr;
    }
    auto r = lm::VirtualPointerManager::create(*disp);
    if (!r) {
      error = to_rust(r.error().message);
      return nullptr;
    }
    auto impl = std::make_unique<VirtualPointerManagerShim::Impl>();
    impl->mgr.emplace(std::move(*r));
    auto *raw = impl.get();
    raw->on_motion =
        raw->mgr->motion_event().connect([raw](lm::VirtualPointerMotion &e) {
          EventRaw ev =
              event(EventKind::VirtualPointerMotion,
                    reinterpret_cast<std::uintptr_t>(e.seat_resource));
          ev.serial = e.time;
          ev.x = static_cast<std::int32_t>(e.dx * 256.0);
          ev.y = static_cast<std::int32_t>(e.dy * 256.0);
          raw->events.push(std::move(ev));
        });
    raw->on_motion_abs = raw->mgr->motion_absolute_event().connect(
        [raw](lm::VirtualPointerMotionAbsolute &e) {
          EventRaw ev =
              event(EventKind::VirtualPointerMotionAbsolute,
                    reinterpret_cast<std::uintptr_t>(e.seat_resource),
                    reinterpret_cast<std::uintptr_t>(e.output_resource));
          ev.serial = e.time;
          ev.x = static_cast<std::int32_t>(e.x);
          ev.y = static_cast<std::int32_t>(e.y);
          ev.value = e.x_extent;
          ev.text = std::to_string(e.y_extent);
          raw->events.push(std::move(ev));
        });
    raw->on_button =
        raw->mgr->button_event().connect([raw](lm::VirtualPointerButton &e) {
          EventRaw ev =
              event(EventKind::VirtualPointerButton,
                    reinterpret_cast<std::uintptr_t>(e.seat_resource));
          ev.serial = e.time;
          ev.value = e.button;
          ev.flag = e.pressed;
          raw->events.push(std::move(ev));
        });
    raw->on_axis =
        raw->mgr->axis_event().connect([raw](lm::VirtualPointerAxis &e) {
          EventRaw ev =
              event(EventKind::VirtualPointerAxis,
                    reinterpret_cast<std::uintptr_t>(e.seat_resource));
          ev.serial = e.time;
          ev.value = e.axis;
          ev.x = static_cast<std::int32_t>(e.value * 256.0);
          raw->events.push(std::move(ev));
        });
    raw->on_frame =
        raw->mgr->frame_event().connect([raw](lm::VirtualPointerFrame &e) {
          EventRaw ev =
              event(EventKind::VirtualPointerFrame,
                    reinterpret_cast<std::uintptr_t>(e.seat_resource));
          raw->events.push(std::move(ev));
        });
    return std::make_unique<VirtualPointerManagerShim>(std::move(impl));
  } catch (const std::exception &e) {
    error = e.what();
    return nullptr;
  }
}

} // namespace lum
