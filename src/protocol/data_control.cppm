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

#include "detail/wayland_fwd.h"

#include <cstdint>
#include <typeinfo>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "wlr-data-control-unstable-v1-protocol.h"

export module luminaria.desktop:data_control;

import std;

import luminaria;

export namespace luminaria {

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

// --------------------------------------------------------------- implementation
// Implements zwlr_data_control_manager_v1 (version 2, or 1 without a primary
// selection).
//
// Everything here is a bridge. Reading is `DataDeviceManager::selection_changed`
// → a fresh zwlr_data_control_offer_v1 per bound device; writing is a
// zwlr_data_control_source_v1 wrapped in a `SelectionSource` and handed to the
// ordinary clipboard, from where pasting clients see it as any other offer.
//
// Offers are INVALIDATED when the selection moves on. A clipboard manager that
// sat on an old offer and then asked to receive would otherwise be handed the
// new clipboard's contents while believing it had the old one's.

namespace luminaria {

using DcMgr = DataControlManager::Impl;

// Which clipboard an offer refers to. Offers are one-shot views of a selection
// that has since possibly changed, so they carry a validity flag.
struct ControlOffer {
    DcMgr* mgr = nullptr;
    bool primary = false;
    bool valid = true;
};

// External linkage: DataControlManager::Impl holds these.
class ControlSource;

struct DataControlManager::Impl {
    wl_display* display = nullptr;
    WlGlobal global;
    DataDeviceManager* data_device = nullptr;
    PrimarySelectionManager* primary = nullptr;

    std::vector<wl_resource*> devices;
    // Every offer we have handed out, so a selection change can invalidate the
    // previous batch (see the file header).
    std::vector<wl_resource*> offers;
    std::function<bool(wl_client*)> filter;

    Signal<SelectionChange>::Connection selection_conn;
    Signal<SelectionChange>::Connection primary_conn;

    ~Impl() {
        for (wl_resource* r : devices) {
            wl_resource_set_user_data(r, nullptr);
            wl_resource_set_destructor(r, nullptr);
        }
        for (wl_resource* r : offers) {
            if (auto* o = static_cast<ControlOffer*>(wl_resource_get_user_data(r))) {
                o->mgr = nullptr;
            }
        }
        if (display != nullptr && filter) {
            wl_display_set_global_filter(display, nullptr, nullptr);
        }
    }
};

// A zwlr_data_control_source_v1 dressed up as an ordinary clipboard owner.
// Owned by its resource.
class ControlSource final : public SelectionSource {
public:
    ControlSource(DcMgr* mgr, wl_resource* resource) : mgr_(mgr), resource_(resource) {}

    [[nodiscard]] const std::vector<std::string>& mime_types() const noexcept override {
        return mimes_;
    }
    void send(const std::string& mime, int fd) override {
        // libwayland dups the fd into the message; closing it is the caller's.
        zwlr_data_control_source_v1_send_send(resource_, mime.c_str(), fd);
    }
    void cancelled() override {
        if (destroying_) {
            return; // the resource is already going; there is nobody to tell
        }
        zwlr_data_control_source_v1_send_cancelled(resource_);
    }

    DcMgr* mgr_ = nullptr;
    wl_resource* resource_ = nullptr;
    std::vector<std::string> mimes_;
    bool destroying_ = false;
    // A source may be handed to one clipboard, once. The protocol makes a
    // second set_selection/set_primary_selection with it a fatal error.
    bool used_ = false;
};

namespace {

ControlOffer* offer_of(wl_resource* r) {
    return static_cast<ControlOffer*>(wl_resource_get_user_data(r));
}
ControlSource* source_of(wl_resource* r) {
    return static_cast<ControlSource*>(wl_resource_get_user_data(r));
}


// ---- zwlr_data_control_offer_v1 ----
void offer_receive(wl_client*, wl_resource* resource, const char* mime, int32_t fd) {
    ControlOffer* offer = offer_of(resource);
    if (!offer->valid) {
        // The selection moved on. Closing the pipe unread is how the requester
        // learns there is nothing there — better than silently handing over
        // someone else's clipboard.
        close(fd);
        return;
    }
    if (offer->primary) {
        if (offer->mgr->primary != nullptr) {
            offer->mgr->primary->selection_receive(mime, fd);
            return;
        }
        close(fd);
        return;
    }
    offer->mgr->data_device->selection_receive(mime, fd);
}

constexpr struct zwlr_data_control_offer_v1_interface offer_impl = {
    .receive = offer_receive,
    .destroy = resource_destroy_request,
};

void offer_resource_destroy(wl_resource* resource) {
    if (ControlOffer* offer = offer_of(resource)) {
        if (offer->mgr != nullptr) {
            std::erase(offer->mgr->offers, resource);
        }
        delete offer;
    }
}

// ---- zwlr_data_control_source_v1 ----
void source_offer(wl_client*, wl_resource* resource, const char* mime) {
    if (mime != nullptr) {
        source_of(resource)->mimes_.emplace_back(mime);
    }
}

constexpr struct zwlr_data_control_source_v1_interface source_impl = {
    .offer = source_offer,
    .destroy = resource_destroy_request,
};

void source_resource_destroy(wl_resource* resource) {
    ControlSource* source = source_of(resource);
    source->destroying_ = true;
    DcMgr* mgr = source->mgr_;
    // If it still owns a clipboard, take it away before the object goes: the
    // managers hold SelectionSource pointers non-owningly.
    if (mgr->data_device->selection_source() == source) {
        mgr->data_device->set_selection(nullptr);
    }
    if (mgr->primary != nullptr && mgr->primary->selection_source() == source) {
        mgr->primary->set_selection(nullptr);
    }
    delete source;
}

// ---- broadcasting a selection to every data-control device ----
void invalidate_offers(DcMgr* mgr, bool primary) {
    for (wl_resource* offer_resource : mgr->offers) {
        ControlOffer* offer = offer_of(offer_resource);
        if (offer->primary == primary) {
            offer->valid = false;
        }
    }
}

void send_offer_to_device(DcMgr* mgr, wl_resource* device, const std::vector<std::string>& mimes,
                          bool primary) {
    const uint32_t version = wl_resource_get_version(device);
    if (primary && version < ZWLR_DATA_CONTROL_DEVICE_V1_PRIMARY_SELECTION_SINCE_VERSION) {
        return;
    }
    if (mimes.empty()) {
        if (primary) {
            zwlr_data_control_device_v1_send_primary_selection(device, nullptr);
        } else {
            zwlr_data_control_device_v1_send_selection(device, nullptr);
        }
        return;
    }
    wl_resource* offer_resource =
        wl_resource_create(wl_resource_get_client(device), &zwlr_data_control_offer_v1_interface,
                           static_cast<int>(version), 0);
    if (offer_resource == nullptr) {
        return;
    }
    wl_resource_set_implementation(offer_resource, &offer_impl,
                                   new ControlOffer{mgr, primary, true}, offer_resource_destroy);
    mgr->offers.push_back(offer_resource);

    zwlr_data_control_device_v1_send_data_offer(device, offer_resource);
    for (const std::string& mime : mimes) {
        zwlr_data_control_offer_v1_send_offer(offer_resource, mime.c_str());
    }
    if (primary) {
        zwlr_data_control_device_v1_send_primary_selection(device, offer_resource);
    } else {
        zwlr_data_control_device_v1_send_selection(device, offer_resource);
    }
}

void broadcast(DcMgr* mgr, const std::vector<std::string>& mimes, bool primary) {
    invalidate_offers(mgr, primary);
    // Copy: creating offers cannot modify the list, but a client dying mid-loop
    // can, and the cost is a handful of pointers.
    std::vector<wl_resource*> devices = mgr->devices;
    for (wl_resource* device : devices) {
        send_offer_to_device(mgr, device, mimes, primary);
    }
}

// ---- zwlr_data_control_device_v1 ----
void device_set_selection(wl_client*, wl_resource* resource, wl_resource* source_resource) {
    auto* mgr = static_cast<DcMgr*>(wl_resource_get_user_data(resource));
    if (source_resource == nullptr) {
        mgr->data_device->set_selection(nullptr);
        return;
    }
    ControlSource* source = source_of(source_resource);
    if (source->used_) {
        wl_resource_post_error(resource, ZWLR_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE,
                               "this data control source has already been used");
        return;
    }
    source->used_ = true;
    mgr->data_device->set_selection(source);
}

void device_set_primary_selection(wl_client*, wl_resource* resource,
                                  wl_resource* source_resource) {
    auto* mgr = static_cast<DcMgr*>(wl_resource_get_user_data(resource));
    if (mgr->primary == nullptr) {
        return; // bound at v1, ignore
    }
    if (source_resource == nullptr) {
        mgr->primary->set_selection(nullptr);
        return;
    }
    ControlSource* source = source_of(source_resource);
    if (source->used_) {
        wl_resource_post_error(resource, ZWLR_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE,
                               "this data control source has already been used");
        return;
    }
    source->used_ = true;
    mgr->primary->set_selection(source);
}

constexpr struct zwlr_data_control_device_v1_interface device_impl = {
    .set_selection = device_set_selection,
    .destroy = resource_destroy_request,
    .set_primary_selection = device_set_primary_selection,
};

void device_resource_destroy(wl_resource* resource) {
    if (auto* mgr = static_cast<DcMgr*>(wl_resource_get_user_data(resource))) {
        std::erase(mgr->devices, resource);
    }
}

// ---- zwlr_data_control_manager_v1 ----
void manager_create_data_source(wl_client* client, wl_resource* manager_resource, uint32_t id) {
    auto* mgr = static_cast<DcMgr*>(wl_resource_get_user_data(manager_resource));
    wl_resource* resource =
        wl_resource_create(client, &zwlr_data_control_source_v1_interface,
                           wl_resource_get_version(manager_resource), static_cast<int>(id));
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &source_impl, new ControlSource(mgr, resource),
                                   source_resource_destroy);
}

void manager_get_data_device(wl_client* client, wl_resource* manager_resource, uint32_t id,
                             wl_resource* /*seat*/) {
    // luminaria has one seat, so which one the client named adds nothing.
    auto* mgr = static_cast<DcMgr*>(wl_resource_get_user_data(manager_resource));
    wl_resource* resource =
        wl_resource_create(client, &zwlr_data_control_device_v1_interface,
                           wl_resource_get_version(manager_resource), static_cast<int>(id));
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &device_impl, mgr, device_resource_destroy);
    mgr->devices.push_back(resource);

    // A clipboard manager binds long after the selection was set; tell it what
    // is on the clipboard now rather than making it wait for the next change.
    send_offer_to_device(mgr, resource, mgr->data_device->selection_mime_types(), false);
    if (mgr->primary != nullptr) {
        send_offer_to_device(mgr, resource, mgr->primary->selection_mime_types(), true);
    }
}

constexpr struct zwlr_data_control_manager_v1_interface manager_impl = {
    .create_data_source = manager_create_data_source,
    .get_data_device = manager_get_data_device,
    .destroy = resource_destroy_request,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zwlr_data_control_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

bool global_filter(const wl_client* client, const wl_global* global, void* data) {
    auto* mgr = static_cast<DcMgr*>(data);
    if (global != mgr->global.get()) {
        return true; // not ours; every other global is none of our business
    }
    return mgr->filter(const_cast<wl_client*>(client));
}

} // namespace

DataControlManager::DataControlManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
DataControlManager::~DataControlManager() = default;
DataControlManager::DataControlManager(DataControlManager&&) noexcept = default;
DataControlManager& DataControlManager::operator=(DataControlManager&&) noexcept = default;

Result<DataControlManager> DataControlManager::create(Display& display,
                                                      DataDeviceManager& data_device,
                                                      PrimarySelectionManager* primary) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->data_device = &data_device;
    impl->primary = primary;
    // Version 2 is what adds the primary selection; advertising it without one
    // would promise a clipboard we cannot deliver.
    const uint32_t version = primary != nullptr ? 2 : 1;
    auto global = create_wl_global<&zwlr_data_control_manager_v1_interface, manager_bind>(
        display, version, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);

    Impl* raw = impl.get();
    impl->selection_conn = data_device.selection_changed().connect(
        [raw](SelectionChange& e) { broadcast(raw, e.mime_types, false); });
    if (primary != nullptr) {
        impl->primary_conn = primary->selection_changed().connect(
            [raw](SelectionChange& e) { broadcast(raw, e.mime_types, true); });
    }
    return DataControlManager{std::move(impl)};
}

void DataControlManager::set_filter(std::function<bool(wl_client*)> filter) {
    impl_->filter = std::move(filter);
    if (impl_->filter) {
        wl_display_set_global_filter(impl_->display, global_filter, impl_.get());
    } else {
        wl_display_set_global_filter(impl_->display, nullptr, nullptr);
    }
}

} // namespace luminaria
