// luminaria/data_control.cppm — zwlr_data_control_manager_v1: the clipboard
// without a window.
//
// `wl-copy`, `wl-paste`, `clipman` and every clipboard-history applet need to
// read and write the selection while having no surface and no keyboard focus —
// which is exactly what wl_data_device refuses to allow, because that rule is
// what stops a background application from reading your passwords. This
// protocol is the deliberate escape hatch: a client that binds it sees every
// selection change as it happens and can set the selection at will.
//
// So it is PRIVILEGED. There is no security in the protocol itself; the
// compositor is expected to expose the global only to clients it trusts.
// Luminaria advertises it to everyone by default and gives you a filter:
//
//     data_control->set_filter([](wl_client* c) { return is_trusted(c); });
//
// Both clipboards are covered — the ordinary selection and, when a
// PrimarySelectionManager exists, the middle-click one (that is the difference
// between advertising version 1 and version 2).
//
// Both managers must outlive this object; it holds them by reference and
// bridges their selections to `SelectionSource` (see :data_device).

module;

#include "luminaria/detail/wayland_fwd.h"

#include <functional>
#include <memory>

export module luminaria:data_control;

import :core.expected;

export namespace luminaria {

class Display;
class DataDeviceManager;
class PrimarySelectionManager;

/// The zwlr_data_control_manager_v1 global. Move-only; pointer-stable state.
class DataControlManager {
public:
    /// Create the global against the two clipboards. `primary` may be null, in
    /// which case version 1 is advertised and the middle-click selection is
    /// simply not exposed.
    [[nodiscard]] static Result<DataControlManager> create(Display& display,
                                                           DataDeviceManager& data_device,
                                                           PrimarySelectionManager* primary);

    ~DataControlManager();
    DataControlManager(DataControlManager&&) noexcept;
    DataControlManager& operator=(DataControlManager&&) noexcept;
    DataControlManager(const DataControlManager&) = delete;
    DataControlManager& operator=(const DataControlManager&) = delete;

    /// Decide which clients may see the global at all. Return false and the
    /// client never learns it exists — it is filtered out of the registry, so
    /// the toolkit's "is it there?" check answers no rather than failing later.
    /// The default admits everyone, which is fine for a single-user session and
    /// wrong for a sandbox.
    ///
    /// libwayland has exactly ONE global filter per wl_display, and this
    /// installs it (every other global passes through untouched). It cannot be
    /// chained — libwayland does not hand the old one back — so a compositor
    /// that needs its own filter for other globals should set that one and do
    /// the data-control check inside it rather than calling this.
    void set_filter(std::function<bool(wl_client*)> filter);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit DataControlManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
