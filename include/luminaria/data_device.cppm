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
#include <string>
#include <vector>

export module luminaria:data_device;

import :core.expected;
import :core.signal;

export namespace luminaria {

class Display;
class Seat;

/// Whoever currently owns a selection. Ordinarily that is a client's
/// wl_data_source and this type never appears; it exists so that code OUTSIDE
/// the protocol can put something on the clipboard — a data-control client
/// (see :data_control), a clipboard manager, an X11 bridge — and have pasting
/// clients see it as an ordinary offer.
///
/// The manager holds it non-owningly and never reads the bytes: `send` gets the
/// pasting client's pipe and the two ends move the data between them.
class SelectionSource {
public:
    virtual ~SelectionSource() = default;

    /// Formats on offer, most-preferred first.
    [[nodiscard]] virtual const std::vector<std::string>& mime_types() const noexcept = 0;
    /// Write the data for `mime` into `fd`. The fd is BORROWED for the duration
    /// of the call — hand it to a client (libwayland dups it into the message)
    /// or write to it here, but do not close it; the caller does.
    virtual void send(const std::string& mime, int fd) = 0;
    /// Something else took the clipboard; this source is no longer it.
    virtual void cancelled() {}
};

/// The clipboard changed hands. `mime_types` is empty when it was cleared.
struct SelectionChange {
    const std::vector<std::string>& mime_types;
};

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

    // --- the clipboard, from outside the protocol ---

    /// Fires whenever the selection changes owner, whichever side set it.
    [[nodiscard]] Signal<SelectionChange>& selection_changed() noexcept;
    /// Formats the current selection offers; empty when the clipboard is empty.
    [[nodiscard]] const std::vector<std::string>& selection_mime_types() const noexcept;
    /// Ask the current owner to write `mime` into `fd`. Takes ownership of `fd`
    /// either way; returns false (and closes it) if there is no selection.
    bool selection_receive(const std::string& mime, int fd);

    /// Put `source` on the clipboard, cancelling whatever held it. The manager
    /// does NOT take ownership: keep it alive until it is replaced (you will be
    /// told via `SelectionSource::cancelled`) or until you clear the selection
    /// with a null pointer.
    void set_selection(SelectionSource* source);
    /// The external source currently holding the clipboard, or null — which
    /// also means null when an ordinary client owns it.
    [[nodiscard]] SelectionSource* selection_source() const noexcept;

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

    /// Same three hooks as DataDeviceManager, for the middle-click selection.
    [[nodiscard]] Signal<SelectionChange>& selection_changed() noexcept;
    [[nodiscard]] const std::vector<std::string>& selection_mime_types() const noexcept;
    bool selection_receive(const std::string& mime, int fd);
    void set_selection(SelectionSource* source);
    [[nodiscard]] SelectionSource* selection_source() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit PrimarySelectionManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
