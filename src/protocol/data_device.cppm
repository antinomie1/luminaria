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


#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "primary-selection-unstable-v1-protocol.h"

export module luminaria:data_device;

import std;

import :compositor;
import :display;
import :expected;
import :protocol_helper;
import :seat;
import :signal;

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

// --------------------------------------------------------------- implementation
namespace luminaria {

namespace {

uint32_t now_ms() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

// ===========================================================================
// wl_data_device_manager — clipboard + drag-and-drop
// ===========================================================================

// Defined below; declared here so Impl can hold pointers to them.
struct Source;
struct Offer;

struct DataDeviceManager::Impl {
    wl_display* display = nullptr;
    WlGlobal global;
    Seat* seat = nullptr;

    std::vector<wl_resource*> devices; // every bound wl_data_device
    Signal<SeatKeyboardFocus>::Connection focus_conn;

    Source* selection = nullptr;
    // A selection set from outside the protocol (a data-control client, a
    // clipboard manager). We own the Source wrapping it, but not the
    // SelectionSource itself.
    Source* external_selection = nullptr;
    Signal<SelectionChange> selection_changed;
    const std::vector<std::string> no_mimes;

    // Drag state (invalid ids / null resources when no drag is running).
    Source* drag_source = nullptr;
    SurfaceId drag_icon;
    SurfaceId drag_focus;              // surface under the cursor
    wl_resource* drag_offer = nullptr; // offer handed to drag_focus's client
    Signal<SurfaceInvalidated>::Connection surface_invalidated_conn;
};

using DdMgr = DataDeviceManager::Impl;

// Owned by its wl_data_source resource.
struct Source {
    // Exactly one of these is set: `resource` for an ordinary client's
    // wl_data_source, `external` for a selection that came from outside the
    // protocol. Everything downstream goes through source_send/source_cancel so
    // the rest of this file does not have to care which.
    wl_resource* resource = nullptr;
    SelectionSource* external = nullptr;
    DdMgr* mgr = nullptr;
    std::vector<std::string> mimes;
    uint32_t dnd_actions = 0;
    bool is_selection = false;
    bool is_drag = false;
    std::vector<wl_resource*> offers; // live wl_data_offers referring to us
};

// Owned by its wl_data_offer resource. `source` is nulled if the source dies.
struct Offer {
    Source* source = nullptr;
    DdMgr* mgr = nullptr;
    bool is_drag = false;
};

namespace {

Source* source_of(wl_resource* r) {
    return static_cast<Source*>(wl_resource_get_user_data(r));
}
Offer* offer_of(wl_resource* r) {
    return static_cast<Offer*>(wl_resource_get_user_data(r));
}

void end_drag(DdMgr* mgr, bool cancelled);

// --- the one place that knows a source might not be a client's ---
void source_send(Source* source, const char* mime, int fd) {
    if (source->external != nullptr) {
        source->external->send(mime, fd);
    } else {
        wl_data_source_send_send(source->resource, mime, fd);
    }
}
void source_cancel(Source* source) {
    if (source->external != nullptr) {
        source->external->cancelled();
    } else {
        wl_data_source_send_cancelled(source->resource);
    }
}

// ---- wl_data_offer ----
void offer_accept(wl_client*, wl_resource* resource, uint32_t, const char* mime) {
    Offer* offer = offer_of(resource);
    // An external source has no `target` event to receive; it is a clipboard
    // owner, and only drag-and-drop uses target.
    if (offer->source != nullptr && offer->source->external == nullptr) {
        wl_data_source_send_target(offer->source->resource, mime);
    }
}
void offer_receive(wl_client*, wl_resource* resource, const char* mime, int32_t fd) {
    Offer* offer = offer_of(resource);
    if (offer->source != nullptr) {
        // Hand the pipe straight to the owning client; the bytes never touch us.
        source_send(offer->source, mime, fd);
    }
    close(fd);
}
void offer_finish(wl_client*, wl_resource* resource) {
    Offer* offer = offer_of(resource);
    if (offer->source != nullptr && offer->source->external == nullptr && offer->is_drag &&
        wl_resource_get_version(offer->source->resource) >=
            WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION) {
        wl_data_source_send_dnd_finished(offer->source->resource);
    }
}
void offer_set_actions(wl_client*, wl_resource* resource, uint32_t actions,
                       uint32_t preferred_action) {
    Offer* offer = offer_of(resource);
    uint32_t chosen = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    const uint32_t available =
        offer->source != nullptr ? (actions & offer->source->dnd_actions) : 0;
    if ((available & preferred_action) != 0) {
        chosen = preferred_action;
    } else if ((available & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) != 0) {
        chosen = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    } else if ((available & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE) != 0) {
        chosen = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    }
    if (wl_resource_get_version(resource) >= WL_DATA_OFFER_ACTION_SINCE_VERSION) {
        wl_data_offer_send_action(resource, chosen);
    }
    if (offer->source != nullptr && offer->source->external == nullptr &&
        wl_resource_get_version(offer->source->resource) >=
            WL_DATA_SOURCE_ACTION_SINCE_VERSION) {
        wl_data_source_send_action(offer->source->resource, chosen);
    }
}
constexpr struct wl_data_offer_interface offer_impl = {
    .accept = offer_accept,
    .receive = offer_receive,
    .destroy = resource_destroy_request,
    .finish = offer_finish,
    .set_actions = offer_set_actions,
};
void offer_resource_destroy(wl_resource* resource) {
    Offer* offer = offer_of(resource);
    if (offer->source != nullptr) {
        std::erase(offer->source->offers, resource);
    }
    if (offer->mgr != nullptr && offer->mgr->drag_offer == resource) {
        offer->mgr->drag_offer = nullptr;
    }
    delete offer;
}

wl_resource* make_offer(DdMgr* mgr, wl_resource* device, Source* source, bool is_drag) {
    wl_client* client = wl_resource_get_client(device);
    wl_resource* offer_resource = wl_resource_create(client, &wl_data_offer_interface,
                                                     wl_resource_get_version(device), 0);
    if (offer_resource == nullptr) {
        return nullptr;
    }
    auto* offer = new Offer{source, mgr, is_drag};
    wl_resource_set_implementation(offer_resource, &offer_impl, offer, offer_resource_destroy);
    source->offers.push_back(offer_resource);

    wl_data_device_send_data_offer(device, offer_resource);
    for (const std::string& mime : source->mimes) {
        wl_data_offer_send_offer(offer_resource, mime.c_str());
    }
    if (is_drag && wl_resource_get_version(offer_resource) >=
                       WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION) {
        wl_data_offer_send_source_actions(offer_resource, source->dnd_actions);
    }
    return offer_resource;
}

// ---- selection plumbing ----
void send_selection_to_client(DdMgr* mgr, wl_client* client) {
    for (wl_resource* device : mgr->devices) {
        if (wl_resource_get_client(device) != client) {
            continue;
        }
        if (mgr->selection == nullptr) {
            wl_data_device_send_selection(device, nullptr);
            continue;
        }
        wl_resource* offer = make_offer(mgr, device, mgr->selection, false);
        wl_data_device_send_selection(device, offer);
    }
}

void broadcast_selection(DdMgr* mgr) {
    Surface* focus = surface_from_id(mgr->seat->keyboard_focus());
    if (focus == nullptr) {
        return; // nobody is focused; the next focus change will deliver it
    }
    send_selection_to_client(mgr, wl_resource_get_client(focus->c_resource()));
}

void apply_selection(DdMgr* mgr, Source* source) {
    if (mgr->selection == source) {
        return;
    }
    if (mgr->selection != nullptr) {
        Source* old = mgr->selection;
        old->is_selection = false;
        source_cancel(old);
        if (old == mgr->external_selection) {
            // We allocated this wrapper; the SelectionSource behind it belongs
            // to the caller and has just been told it lost the clipboard.
            for (wl_resource* offer : old->offers) {
                offer_of(offer)->source = nullptr;
            }
            mgr->external_selection = nullptr;
            delete old;
        }
    }
    mgr->selection = source;
    if (source != nullptr) {
        source->is_selection = true;
    }
    broadcast_selection(mgr);
    SelectionChange event{source != nullptr ? source->mimes : mgr->no_mimes};
    mgr->selection_changed.emit(event);
}

// ---- wl_data_source ----
void source_offer(wl_client*, wl_resource* resource, const char* mime) {
    if (mime != nullptr) {
        source_of(resource)->mimes.emplace_back(mime);
    }
}
void source_set_actions(wl_client*, wl_resource* resource, uint32_t actions) {
    source_of(resource)->dnd_actions = actions;
}
constexpr struct wl_data_source_interface source_impl = {
    .offer = source_offer,
    .destroy = resource_destroy_request,
    .set_actions = source_set_actions,
};
void source_resource_destroy(wl_resource* resource) {
    Source* source = source_of(resource);
    DdMgr* mgr = source->mgr;
    // Offers can outlive the source; make them inert rather than dangling.
    for (wl_resource* offer : source->offers) {
        offer_of(offer)->source = nullptr;
    }
    if (mgr != nullptr && mgr->selection == source) {
        mgr->selection = nullptr;
        broadcast_selection(mgr); // tells the focused client the clipboard is empty
        SelectionChange event{mgr->no_mimes};
        mgr->selection_changed.emit(event);
    }
    if (mgr != nullptr && mgr->drag_source == source) {
        mgr->drag_source = nullptr;
        end_drag(mgr, true);
    }
    delete source;
}

// ---- drag and drop ----
void drag_send_leave(DdMgr* mgr) {
    if (!mgr->drag_focus.valid()) {
        return;
    }
    if (Surface* focus = surface_from_id(mgr->drag_focus); focus != nullptr) {
        wl_client* client = wl_resource_get_client(focus->c_resource());
        for (wl_resource* device : mgr->devices) {
            if (wl_resource_get_client(device) == client) {
                wl_data_device_send_leave(device);
            }
        }
    }
    if (mgr->drag_offer != nullptr) {
        wl_resource* offer = mgr->drag_offer;
        mgr->drag_offer = nullptr;
        wl_resource_destroy(offer);
    }
    mgr->drag_focus = {};
}

void drag_focus(DdMgr* mgr, SurfaceId surface_id, double sx, double sy) {
    if (mgr->drag_focus == surface_id) {
        return;
    }
    drag_send_leave(mgr);
    Surface* surface = surface_from_id(surface_id);
    if (surface == nullptr || mgr->drag_source == nullptr) {
        return;
    }
    wl_resource* surface_resource = surface->c_resource();
    wl_client* client = wl_resource_get_client(surface_resource);
    const uint32_t serial = wl_display_next_serial(mgr->display);
    for (wl_resource* device : mgr->devices) {
        if (wl_resource_get_client(device) != client) {
            continue;
        }
        wl_resource* offer = make_offer(mgr, device, mgr->drag_source, true);
        mgr->drag_offer = offer;
        wl_data_device_send_enter(device, serial, surface_resource, wl_fixed_from_double(sx),
                                  wl_fixed_from_double(sy), offer);
    }
    mgr->drag_focus = surface_id;
}

void drag_motion(DdMgr* mgr, double sx, double sy) {
    Surface* focus = surface_from_id(mgr->drag_focus);
    if (focus == nullptr) {
        return;
    }
    wl_client* client = wl_resource_get_client(focus->c_resource());
    const uint32_t time = now_ms();
    for (wl_resource* device : mgr->devices) {
        if (wl_resource_get_client(device) == client) {
            wl_data_device_send_motion(device, time, wl_fixed_from_double(sx),
                                       wl_fixed_from_double(sy));
        }
    }
}

void end_drag(DdMgr* mgr, bool cancelled) {
    if (cancelled) {
        drag_send_leave(mgr);
        if (mgr->drag_source != nullptr) {
            wl_data_source_send_cancelled(mgr->drag_source->resource);
        }
    }
    if (mgr->drag_source != nullptr) {
        mgr->drag_source->is_drag = false;
        mgr->drag_source = nullptr;
    }
    mgr->drag_icon = {};
    mgr->drag_focus = {};
    mgr->drag_offer = nullptr;
    mgr->seat->end_drag();
}

void drag_drop(DdMgr* mgr) {
    Surface* focus = surface_from_id(mgr->drag_focus);
    if (focus == nullptr || mgr->drag_source == nullptr) {
        end_drag(mgr, true);
        return;
    }
    wl_client* client = wl_resource_get_client(focus->c_resource());
    for (wl_resource* device : mgr->devices) {
        if (wl_resource_get_client(device) == client) {
            wl_data_device_send_drop(device);
        }
    }
    if (wl_resource_get_version(mgr->drag_source->resource) >=
        WL_DATA_SOURCE_DND_DROP_PERFORMED_SINCE_VERSION) {
        wl_data_source_send_dnd_drop_performed(mgr->drag_source->resource);
    }
    // The offer stays alive: the receiving client still has to pull the data.
    if (mgr->drag_source != nullptr) {
        mgr->drag_source->is_drag = false;
        mgr->drag_source = nullptr;
    }
    mgr->drag_icon = {};
    mgr->drag_focus = {};
    mgr->drag_offer = nullptr;
    mgr->seat->end_drag();
}

// ---- wl_data_device ----
void device_start_drag(wl_client*, wl_resource* resource, wl_resource* source_resource,
                       wl_resource* /*origin*/, wl_resource* icon, uint32_t /*serial*/) {
    auto* mgr = static_cast<DdMgr*>(wl_resource_get_user_data(resource));
    if (mgr->drag_source != nullptr) {
        return; // a drag is already running
    }
    if (source_resource == nullptr) {
        return; // drag with no data: nothing to offer, so nothing to do
    }
    Source* source = source_of(source_resource);
    source->is_drag = true;
    mgr->drag_source = source;
    Surface* drag_icon = icon != nullptr ? surface_from_resource(icon) : nullptr;
    if (drag_icon != nullptr) {
        // Drawn under the pointer for the whole drag, so it must not be what
        // the hit test finds there — the drop target is.
        drag_icon->set_input_transparent();
    }
    mgr->drag_icon = drag_icon != nullptr ? drag_icon->id() : SurfaceId{};

    SeatDragHooks hooks;
    hooks.focus = [mgr](SurfaceId surface, double sx, double sy) {
        drag_focus(mgr, surface, sx, sy);
    };
    hooks.motion = [mgr](double sx, double sy) { drag_motion(mgr, sx, sy); };
    hooks.drop = [mgr] { drag_drop(mgr); };
    mgr->seat->begin_drag(std::move(hooks));
}

void device_set_selection(wl_client*, wl_resource* resource, wl_resource* source_resource,
                          uint32_t /*serial*/) {
    auto* mgr = static_cast<DdMgr*>(wl_resource_get_user_data(resource));
    apply_selection(mgr, source_resource != nullptr ? source_of(source_resource) : nullptr);
}

constexpr struct wl_data_device_interface device_impl = {
    .start_drag = device_start_drag,
    .set_selection = device_set_selection,
    .release = resource_destroy_request,
};

void device_resource_destroy(wl_resource* resource) {
    auto* mgr = static_cast<DdMgr*>(wl_resource_get_user_data(resource));
    std::erase(mgr->devices, resource);
}

// ---- wl_data_device_manager ----
void manager_create_data_source(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* mgr = static_cast<DdMgr*>(wl_resource_get_user_data(resource));
    wl_resource* source_resource = wl_resource_create(client, &wl_data_source_interface,
                                                      wl_resource_get_version(resource), id);
    if (source_resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* source = new Source{};
    source->resource = source_resource;
    source->mgr = mgr;
    wl_resource_set_implementation(source_resource, &source_impl, source, source_resource_destroy);
}

void manager_get_data_device(wl_client* client, wl_resource* resource, uint32_t id,
                             wl_resource* /*seat*/) {
    auto* mgr = static_cast<DdMgr*>(wl_resource_get_user_data(resource));
    wl_resource* device = wl_resource_create(client, &wl_data_device_interface,
                                             wl_resource_get_version(resource), id);
    if (device == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(device, &device_impl, mgr, device_resource_destroy);
    mgr->devices.push_back(device);
    // If this client already holds focus, hand it the clipboard right away.
    Surface* focus = surface_from_id(mgr->seat->keyboard_focus());
    if (focus != nullptr && wl_resource_get_client(focus->c_resource()) == client) {
        send_selection_to_client(mgr, client);
    }
}

constexpr struct wl_data_device_manager_interface manager_impl = {
    .create_data_source = manager_create_data_source,
    .get_data_device = manager_get_data_device,
};

} // namespace

DataDeviceManager::DataDeviceManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
DataDeviceManager::~DataDeviceManager() = default;
DataDeviceManager::DataDeviceManager(DataDeviceManager&&) noexcept = default;
DataDeviceManager& DataDeviceManager::operator=(DataDeviceManager&&) noexcept = default;

Result<DataDeviceManager> DataDeviceManager::create(Display& display, Seat& seat) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->seat = &seat;
    auto global = create_wl_global<&wl_data_device_manager_interface,
                                   default_bind<&wl_data_device_manager_interface,
                                                &manager_impl>>(display, 3, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    Impl* raw = impl.get();
    impl->focus_conn = seat.keyboard_focus_changed().connect([raw](SeatKeyboardFocus& e) {
        // The clipboard follows keyboard focus: the newly focused client is the
        // one allowed to see (and paste) the selection.
        if (Surface* surface = surface_from_id(e.surface); surface != nullptr) {
            send_selection_to_client(raw, wl_resource_get_client(surface->c_resource()));
        }
    });
    impl->surface_invalidated_conn =
        surface_invalidated().connect([raw](SurfaceInvalidated& event) {
            if (raw->drag_icon == event.surface) {
                raw->drag_icon = {};
            }
            if (raw->drag_focus != event.surface) {
                return;
            }
            raw->drag_focus = {};
            if (raw->drag_offer != nullptr) {
                wl_resource* offer = raw->drag_offer;
                raw->drag_offer = nullptr;
                wl_resource_destroy(offer);
            }
        });
    return DataDeviceManager{std::move(impl)};
}

bool DataDeviceManager::dragging() const noexcept {
    return impl_->drag_source != nullptr;
}

Signal<SelectionChange>& DataDeviceManager::selection_changed() noexcept {
    return impl_->selection_changed;
}

const std::vector<std::string>& DataDeviceManager::selection_mime_types() const noexcept {
    return impl_->selection != nullptr ? impl_->selection->mimes : impl_->no_mimes;
}

bool DataDeviceManager::selection_receive(const std::string& mime, int fd) {
    if (impl_->selection == nullptr) {
        close(fd);
        return false;
    }
    source_send(impl_->selection, mime.c_str(), fd);
    close(fd);
    return true;
}

void DataDeviceManager::set_selection(SelectionSource* source) {
    if (source == nullptr) {
        apply_selection(impl_.get(), nullptr);
        return;
    }
    // The wrapper is ours; the SelectionSource behind it is the caller's. Note
    // the order: apply_selection compares the OUTGOING selection against
    // external_selection to decide whether to free it, so the new wrapper is
    // recorded only afterwards.
    auto* wrapper = new Source{};
    wrapper->external = source;
    wrapper->mgr = impl_.get();
    wrapper->mimes = source->mime_types();
    apply_selection(impl_.get(), wrapper);
    impl_->external_selection = wrapper;
}

SelectionSource* DataDeviceManager::selection_source() const noexcept {
    return impl_->selection != nullptr ? impl_->selection->external : nullptr;
}

// ===========================================================================
// zwp_primary_selection_device_manager_v1 — middle-click paste
// ===========================================================================

struct PrimarySource;
struct PrimaryOffer;

struct PrimarySelectionManager::Impl {
    wl_display* display = nullptr;
    WlGlobal global;
    Seat* seat = nullptr;
    std::vector<wl_resource*> devices;
    Signal<SeatKeyboardFocus>::Connection focus_conn;

    PrimarySource* selection = nullptr;
    PrimarySource* external_selection = nullptr;
    Signal<SelectionChange> selection_changed;
    const std::vector<std::string> no_mimes;
};

using PrimaryMgr = PrimarySelectionManager::Impl;

struct PrimarySource {
    // Same split as Source above: a client's zwp_primary_selection_source_v1,
    // or a selection handed to us from outside the protocol.
    wl_resource* resource = nullptr;
    SelectionSource* external = nullptr;
    PrimaryMgr* mgr = nullptr;
    std::vector<std::string> mimes;
    std::vector<wl_resource*> offers;
};

struct PrimaryOffer {
    PrimarySource* source = nullptr;
};

namespace {

void primary_source_send(PrimarySource* source, const char* mime, int fd) {
    if (source->external != nullptr) {
        source->external->send(mime, fd);
    } else {
        zwp_primary_selection_source_v1_send_send(source->resource, mime, fd);
    }
}
void primary_source_cancel(PrimarySource* source) {
    if (source->external != nullptr) {
        source->external->cancelled();
    } else {
        zwp_primary_selection_source_v1_send_cancelled(source->resource);
    }
}

PrimarySource* primary_source_of(wl_resource* r) {
    return static_cast<PrimarySource*>(wl_resource_get_user_data(r));
}
PrimaryOffer* primary_offer_of(wl_resource* r) {
    return static_cast<PrimaryOffer*>(wl_resource_get_user_data(r));
}

void primary_offer_receive(wl_client*, wl_resource* resource, const char* mime, int32_t fd) {
    PrimaryOffer* offer = primary_offer_of(resource);
    if (offer->source != nullptr) {
        primary_source_send(offer->source, mime, fd);
    }
    close(fd);
}
constexpr struct zwp_primary_selection_offer_v1_interface primary_offer_impl = {
    .receive = primary_offer_receive,
    .destroy = resource_destroy_request,
};
void primary_offer_resource_destroy(wl_resource* resource) {
    PrimaryOffer* offer = primary_offer_of(resource);
    if (offer->source != nullptr) {
        std::erase(offer->source->offers, resource);
    }
    delete offer;
}

void primary_send_selection_to_client(PrimaryMgr* mgr, wl_client* client) {
    for (wl_resource* device : mgr->devices) {
        if (wl_resource_get_client(device) != client) {
            continue;
        }
        if (mgr->selection == nullptr) {
            zwp_primary_selection_device_v1_send_selection(device, nullptr);
            continue;
        }
        wl_resource* offer_resource =
            wl_resource_create(client, &zwp_primary_selection_offer_v1_interface,
                               wl_resource_get_version(device), 0);
        if (offer_resource == nullptr) {
            continue;
        }
        auto* offer = new PrimaryOffer{mgr->selection};
        wl_resource_set_implementation(offer_resource, &primary_offer_impl, offer,
                                       primary_offer_resource_destroy);
        mgr->selection->offers.push_back(offer_resource);
        zwp_primary_selection_device_v1_send_data_offer(device, offer_resource);
        for (const std::string& mime : mgr->selection->mimes) {
            zwp_primary_selection_offer_v1_send_offer(offer_resource, mime.c_str());
        }
        zwp_primary_selection_device_v1_send_selection(device, offer_resource);
    }
}

void primary_broadcast(PrimaryMgr* mgr) {
    Surface* focus = surface_from_id(mgr->seat->keyboard_focus());
    if (focus != nullptr) {
        primary_send_selection_to_client(mgr, wl_resource_get_client(focus->c_resource()));
    }
}

void primary_source_offer(wl_client*, wl_resource* resource, const char* mime) {
    if (mime != nullptr) {
        primary_source_of(resource)->mimes.emplace_back(mime);
    }
}
constexpr struct zwp_primary_selection_source_v1_interface primary_source_impl = {
    .offer = primary_source_offer,
    .destroy = resource_destroy_request,
};
void primary_source_resource_destroy(wl_resource* resource) {
    PrimarySource* source = primary_source_of(resource);
    for (wl_resource* offer : source->offers) {
        primary_offer_of(offer)->source = nullptr;
    }
    if (source->mgr != nullptr && source->mgr->selection == source) {
        source->mgr->selection = nullptr;
        primary_broadcast(source->mgr);
        SelectionChange event{source->mgr->no_mimes};
        source->mgr->selection_changed.emit(event);
    }
    delete source;
}

void primary_set_selection(PrimaryMgr* mgr, PrimarySource* source) {
    if (mgr->selection == source) {
        return;
    }
    if (mgr->selection != nullptr) {
        PrimarySource* old = mgr->selection;
        primary_source_cancel(old);
        if (old == mgr->external_selection) {
            for (wl_resource* offer : old->offers) {
                primary_offer_of(offer)->source = nullptr;
            }
            mgr->external_selection = nullptr;
            delete old;
        }
    }
    mgr->selection = source;
    primary_broadcast(mgr);
    SelectionChange event{source != nullptr ? source->mimes : mgr->no_mimes};
    mgr->selection_changed.emit(event);
}

void primary_device_set_selection(wl_client*, wl_resource* resource, wl_resource* source_resource,
                                  uint32_t /*serial*/) {
    auto* mgr = static_cast<PrimaryMgr*>(wl_resource_get_user_data(resource));
    primary_set_selection(mgr,
                          source_resource != nullptr ? primary_source_of(source_resource) : nullptr);
}
constexpr struct zwp_primary_selection_device_v1_interface primary_device_impl = {
    .set_selection = primary_device_set_selection,
    .destroy = resource_destroy_request,
};
void primary_device_resource_destroy(wl_resource* resource) {
    auto* mgr = static_cast<PrimaryMgr*>(wl_resource_get_user_data(resource));
    std::erase(mgr->devices, resource);
}

void primary_manager_create_source(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* mgr = static_cast<PrimaryMgr*>(wl_resource_get_user_data(resource));
    wl_resource* source_resource =
        wl_resource_create(client, &zwp_primary_selection_source_v1_interface,
                           wl_resource_get_version(resource), id);
    if (source_resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* source = new PrimarySource{};
    source->resource = source_resource;
    source->mgr = mgr;
    wl_resource_set_implementation(source_resource, &primary_source_impl, source,
                                   primary_source_resource_destroy);
}

void primary_manager_get_device(wl_client* client, wl_resource* resource, uint32_t id,
                                wl_resource* /*seat*/) {
    auto* mgr = static_cast<PrimaryMgr*>(wl_resource_get_user_data(resource));
    wl_resource* device = wl_resource_create(client, &zwp_primary_selection_device_v1_interface,
                                             wl_resource_get_version(resource), id);
    if (device == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(device, &primary_device_impl, mgr,
                                   primary_device_resource_destroy);
    mgr->devices.push_back(device);
    Surface* focus = surface_from_id(mgr->seat->keyboard_focus());
    if (focus != nullptr && wl_resource_get_client(focus->c_resource()) == client) {
        primary_send_selection_to_client(mgr, client);
    }
}

constexpr struct zwp_primary_selection_device_manager_v1_interface primary_manager_impl = {
    .create_source = primary_manager_create_source,
    .get_device = primary_manager_get_device,
    .destroy = resource_destroy_request,
};

} // namespace

PrimarySelectionManager::PrimarySelectionManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
PrimarySelectionManager::~PrimarySelectionManager() = default;
PrimarySelectionManager::PrimarySelectionManager(PrimarySelectionManager&&) noexcept = default;
PrimarySelectionManager&
PrimarySelectionManager::operator=(PrimarySelectionManager&&) noexcept = default;

Result<PrimarySelectionManager> PrimarySelectionManager::create(Display& display, Seat& seat) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->seat = &seat;
    auto global = create_wl_global<&zwp_primary_selection_device_manager_v1_interface,
                                   default_bind<&zwp_primary_selection_device_manager_v1_interface,
                                                &primary_manager_impl>>(display, 1, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    Impl* raw = impl.get();
    impl->focus_conn = seat.keyboard_focus_changed().connect([raw](SeatKeyboardFocus& e) {
        if (Surface* surface = surface_from_id(e.surface); surface != nullptr) {
            primary_send_selection_to_client(raw, wl_resource_get_client(surface->c_resource()));
        }
    });
    return PrimarySelectionManager{std::move(impl)};
}

Signal<SelectionChange>& PrimarySelectionManager::selection_changed() noexcept {
    return impl_->selection_changed;
}

const std::vector<std::string>& PrimarySelectionManager::selection_mime_types() const noexcept {
    return impl_->selection != nullptr ? impl_->selection->mimes : impl_->no_mimes;
}

bool PrimarySelectionManager::selection_receive(const std::string& mime, int fd) {
    if (impl_->selection == nullptr) {
        close(fd);
        return false;
    }
    primary_source_send(impl_->selection, mime.c_str(), fd);
    close(fd);
    return true;
}

void PrimarySelectionManager::set_selection(SelectionSource* source) {
    if (source == nullptr) {
        primary_set_selection(impl_.get(), nullptr);
        return;
    }
    auto* wrapper = new PrimarySource{};
    wrapper->external = source;
    wrapper->mgr = impl_.get();
    wrapper->mimes = source->mime_types();
    primary_set_selection(impl_.get(), wrapper);
    impl_->external_selection = wrapper;
}

SelectionSource* PrimarySelectionManager::selection_source() const noexcept {
    return impl_->selection != nullptr ? impl_->selection->external : nullptr;
}

} // namespace luminaria
