// luminaria/data_device.cppm — clipboard and drag-and-drop.
//
// Two globals, same shape:
//   * DataDeviceManager     — wl_data_device_manager (v3): the CLIPBOARD
//     (Ctrl-C / Ctrl-V) plus drag-and-drop between clients.
//   * PrimarySelectionManager — zwp_primary_selection_device_manager_v1: the
//     X11-style middle-click selection.
//
// Both follow the same rule: the client holding KEYBOARD FOCUS owns the
// selection and is the one offered its contents. Both therefore need a Seat,
// whose focus signal they subscribe to; keep the Seat alive at least as long.
//
// Transfers are zero-copy in the compositor's sense: the receiving client hands
// us a pipe fd, we pass it straight to the source client, and the two of them
// move the bytes without the compositor reading them.

module;

#include <memory>

export module luminaria:data_device;

import :core.expected;

export namespace luminaria {

class Display;
class Seat;

/// wl_data_device_manager (protocol version 3): clipboard + drag-and-drop.
class DataDeviceManager {
public:
    /// Create the global. Drags are driven by `seat`'s pointer focus, and the
    /// selection follows its keyboard focus.
    [[nodiscard]] static Result<DataDeviceManager> create(Display& display, Seat& seat);

    ~DataDeviceManager();
    DataDeviceManager(DataDeviceManager&&) noexcept;
    DataDeviceManager& operator=(DataDeviceManager&&) noexcept;
    DataDeviceManager(const DataDeviceManager&) = delete;
    DataDeviceManager& operator=(const DataDeviceManager&) = delete;

    /// True while a drag-and-drop is in progress.
    [[nodiscard]] bool dragging() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit DataDeviceManager(std::unique_ptr<Impl> impl) noexcept;
};

/// zwp_primary_selection_device_manager_v1: middle-click paste.
class PrimarySelectionManager {
public:
    [[nodiscard]] static Result<PrimarySelectionManager> create(Display& display, Seat& seat);

    ~PrimarySelectionManager();
    PrimarySelectionManager(PrimarySelectionManager&&) noexcept;
    PrimarySelectionManager& operator=(PrimarySelectionManager&&) noexcept;
    PrimarySelectionManager(const PrimarySelectionManager&) = delete;
    PrimarySelectionManager& operator=(const PrimarySelectionManager&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit PrimarySelectionManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
