#include "luminaria/data_device.hpp"

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

#include <unistd.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "primary-selection-unstable-v1-protocol.h"

#include "luminaria/compositor.hpp"
#include "luminaria/core/display.hpp"
#include "luminaria/seat.hpp"

namespace luminaria {

namespace {

uint32_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
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
    wl_global* global = nullptr;
    Seat* seat = nullptr;

    std::vector<wl_resource*> devices; // every bound wl_data_device
    Signal<SeatKeyboardFocus>::Connection focus_conn;

    Source* selection = nullptr;

    // Drag state (all null when no drag is running).
    Source* drag_source = nullptr;
    wl_resource* drag_icon = nullptr;  // wl_surface used as the drag image
    Surface* drag_focus = nullptr;     // surface under the cursor
    wl_resource* drag_offer = nullptr; // offer handed to drag_focus's client

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

using Mgr = DataDeviceManager::Impl;

// Owned by its wl_data_source resource.
struct Source {
    wl_resource* resource = nullptr;
    Mgr* mgr = nullptr;
    std::vector<std::string> mimes;
    uint32_t dnd_actions = 0;
    bool is_selection = false;
    bool is_drag = false;
    std::vector<wl_resource*> offers; // live wl_data_offers referring to us
};

// Owned by its wl_data_offer resource. `source` is nulled if the source dies.
struct Offer {
    Source* source = nullptr;
    Mgr* mgr = nullptr;
    bool is_drag = false;
};

namespace {

Source* source_of(wl_resource* r) {
    return static_cast<Source*>(wl_resource_get_user_data(r));
}
Offer* offer_of(wl_resource* r) {
    return static_cast<Offer*>(wl_resource_get_user_data(r));
}

void end_drag(Mgr* mgr, bool cancelled);

// ---- wl_data_offer ----
void offer_accept(wl_client*, wl_resource* resource, uint32_t, const char* mime) {
    Offer* offer = offer_of(resource);
    if (offer->source != nullptr) {
        wl_data_source_send_target(offer->source->resource, mime);
    }
}
void offer_receive(wl_client*, wl_resource* resource, const char* mime, int32_t fd) {
    Offer* offer = offer_of(resource);
    if (offer->source != nullptr) {
        // Hand the pipe straight to the owning client; the bytes never touch us.
        wl_data_source_send_send(offer->source->resource, mime, fd);
    }
    close(fd);
}
void offer_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void offer_finish(wl_client*, wl_resource* resource) {
    Offer* offer = offer_of(resource);
    if (offer->source != nullptr && offer->is_drag &&
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
    if (offer->source != nullptr && wl_resource_get_version(offer->source->resource) >=
                                        WL_DATA_SOURCE_ACTION_SINCE_VERSION) {
        wl_data_source_send_action(offer->source->resource, chosen);
    }
}
constexpr struct wl_data_offer_interface offer_impl = {
    .accept = offer_accept,
    .receive = offer_receive,
    .destroy = offer_destroy_request,
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

wl_resource* make_offer(Mgr* mgr, wl_resource* device, Source* source, bool is_drag) {
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
void send_selection_to_client(Mgr* mgr, wl_client* client) {
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

void broadcast_selection(Mgr* mgr) {
    Surface* focus = mgr->seat->keyboard_focus();
    if (focus == nullptr) {
        return; // nobody is focused; the next focus change will deliver it
    }
    send_selection_to_client(mgr, wl_resource_get_client(focus->c_resource()));
}

void set_selection(Mgr* mgr, Source* source) {
    if (mgr->selection == source) {
        return;
    }
    if (mgr->selection != nullptr) {
        mgr->selection->is_selection = false;
        wl_data_source_send_cancelled(mgr->selection->resource);
    }
    mgr->selection = source;
    if (source != nullptr) {
        source->is_selection = true;
    }
    broadcast_selection(mgr);
}

// ---- wl_data_source ----
void source_offer(wl_client*, wl_resource* resource, const char* mime) {
    if (mime != nullptr) {
        source_of(resource)->mimes.emplace_back(mime);
    }
}
void source_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void source_set_actions(wl_client*, wl_resource* resource, uint32_t actions) {
    source_of(resource)->dnd_actions = actions;
}
constexpr struct wl_data_source_interface source_impl = {
    .offer = source_offer,
    .destroy = source_destroy_request,
    .set_actions = source_set_actions,
};
void source_resource_destroy(wl_resource* resource) {
    Source* source = source_of(resource);
    Mgr* mgr = source->mgr;
    // Offers can outlive the source; make them inert rather than dangling.
    for (wl_resource* offer : source->offers) {
        offer_of(offer)->source = nullptr;
    }
    if (mgr != nullptr && mgr->selection == source) {
        mgr->selection = nullptr;
        broadcast_selection(mgr); // tells the focused client the clipboard is empty
    }
    if (mgr != nullptr && mgr->drag_source == source) {
        mgr->drag_source = nullptr;
        end_drag(mgr, true);
    }
    delete source;
}

// ---- drag and drop ----
void drag_send_leave(Mgr* mgr) {
    if (mgr->drag_focus == nullptr) {
        return;
    }
    wl_client* client = wl_resource_get_client(mgr->drag_focus->c_resource());
    for (wl_resource* device : mgr->devices) {
        if (wl_resource_get_client(device) == client) {
            wl_data_device_send_leave(device);
        }
    }
    if (mgr->drag_offer != nullptr) {
        wl_resource* offer = mgr->drag_offer;
        mgr->drag_offer = nullptr;
        wl_resource_destroy(offer);
    }
    mgr->drag_focus = nullptr;
}

void drag_focus(Mgr* mgr, Surface* surface, double sx, double sy) {
    if (mgr->drag_focus == surface) {
        return;
    }
    drag_send_leave(mgr);
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
    mgr->drag_focus = surface;
}

void drag_motion(Mgr* mgr, double sx, double sy) {
    if (mgr->drag_focus == nullptr) {
        return;
    }
    wl_client* client = wl_resource_get_client(mgr->drag_focus->c_resource());
    const uint32_t time = now_ms();
    for (wl_resource* device : mgr->devices) {
        if (wl_resource_get_client(device) == client) {
            wl_data_device_send_motion(device, time, wl_fixed_from_double(sx),
                                       wl_fixed_from_double(sy));
        }
    }
}

void end_drag(Mgr* mgr, bool cancelled) {
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
    mgr->drag_icon = nullptr;
    mgr->drag_focus = nullptr;
    mgr->drag_offer = nullptr;
    mgr->seat->end_drag();
}

void drag_drop(Mgr* mgr) {
    if (mgr->drag_focus == nullptr || mgr->drag_source == nullptr) {
        end_drag(mgr, true);
        return;
    }
    wl_client* client = wl_resource_get_client(mgr->drag_focus->c_resource());
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
    mgr->drag_icon = nullptr;
    mgr->drag_focus = nullptr;
    mgr->drag_offer = nullptr;
    mgr->seat->end_drag();
}

// ---- wl_data_device ----
void device_start_drag(wl_client*, wl_resource* resource, wl_resource* source_resource,
                       wl_resource* /*origin*/, wl_resource* icon, uint32_t /*serial*/) {
    auto* mgr = static_cast<Mgr*>(wl_resource_get_user_data(resource));
    if (mgr->drag_source != nullptr) {
        return; // a drag is already running
    }
    if (source_resource == nullptr) {
        return; // drag with no data: nothing to offer, so nothing to do
    }
    Source* source = source_of(source_resource);
    source->is_drag = true;
    mgr->drag_source = source;
    mgr->drag_icon = icon;

    SeatDragHooks hooks;
    hooks.focus = [mgr](Surface* surface, double sx, double sy) {
        drag_focus(mgr, surface, sx, sy);
    };
    hooks.motion = [mgr](double sx, double sy) { drag_motion(mgr, sx, sy); };
    hooks.drop = [mgr] { drag_drop(mgr); };
    mgr->seat->begin_drag(std::move(hooks));
}

void device_set_selection(wl_client*, wl_resource* resource, wl_resource* source_resource,
                          uint32_t /*serial*/) {
    auto* mgr = static_cast<Mgr*>(wl_resource_get_user_data(resource));
    set_selection(mgr, source_resource != nullptr ? source_of(source_resource) : nullptr);
}

void device_release(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

constexpr struct wl_data_device_interface device_impl = {
    .start_drag = device_start_drag,
    .set_selection = device_set_selection,
    .release = device_release,
};

void device_resource_destroy(wl_resource* resource) {
    auto* mgr = static_cast<Mgr*>(wl_resource_get_user_data(resource));
    std::erase(mgr->devices, resource);
}

// ---- wl_data_device_manager ----
void manager_create_data_source(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* mgr = static_cast<Mgr*>(wl_resource_get_user_data(resource));
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
    auto* mgr = static_cast<Mgr*>(wl_resource_get_user_data(resource));
    wl_resource* device = wl_resource_create(client, &wl_data_device_interface,
                                             wl_resource_get_version(resource), id);
    if (device == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(device, &device_impl, mgr, device_resource_destroy);
    mgr->devices.push_back(device);
    // If this client already holds focus, hand it the clipboard right away.
    Surface* focus = mgr->seat->keyboard_focus();
    if (focus != nullptr && wl_resource_get_client(focus->c_resource()) == client) {
        send_selection_to_client(mgr, client);
    }
}

constexpr struct wl_data_device_manager_interface manager_impl = {
    .create_data_source = manager_create_data_source,
    .get_data_device = manager_get_data_device,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_data_device_manager_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

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
    impl->global = wl_global_create(impl->display, &wl_data_device_manager_interface, 3,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wl_data_device_manager) failed");
    }
    Impl* raw = impl.get();
    impl->focus_conn = seat.keyboard_focus_changed().connect([raw](SeatKeyboardFocus& e) {
        // The clipboard follows keyboard focus: the newly focused client is the
        // one allowed to see (and paste) the selection.
        if (e.surface != nullptr) {
            send_selection_to_client(raw, wl_resource_get_client(e.surface->c_resource()));
        }
    });
    return DataDeviceManager{std::move(impl)};
}

bool DataDeviceManager::dragging() const noexcept {
    return impl_->drag_source != nullptr;
}

// ===========================================================================
// zwp_primary_selection_device_manager_v1 — middle-click paste
// ===========================================================================

struct PrimarySource;
struct PrimaryOffer;

struct PrimarySelectionManager::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    Seat* seat = nullptr;
    std::vector<wl_resource*> devices;
    Signal<SeatKeyboardFocus>::Connection focus_conn;

    PrimarySource* selection = nullptr;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

using PrimaryMgr = PrimarySelectionManager::Impl;

struct PrimarySource {
    wl_resource* resource = nullptr;
    PrimaryMgr* mgr = nullptr;
    std::vector<std::string> mimes;
    std::vector<wl_resource*> offers;
};

struct PrimaryOffer {
    PrimarySource* source = nullptr;
};

namespace {

PrimarySource* primary_source_of(wl_resource* r) {
    return static_cast<PrimarySource*>(wl_resource_get_user_data(r));
}
PrimaryOffer* primary_offer_of(wl_resource* r) {
    return static_cast<PrimaryOffer*>(wl_resource_get_user_data(r));
}

void primary_offer_receive(wl_client*, wl_resource* resource, const char* mime, int32_t fd) {
    PrimaryOffer* offer = primary_offer_of(resource);
    if (offer->source != nullptr) {
        zwp_primary_selection_source_v1_send_send(offer->source->resource, mime, fd);
    }
    close(fd);
}
void primary_generic_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
constexpr struct zwp_primary_selection_offer_v1_interface primary_offer_impl = {
    .receive = primary_offer_receive,
    .destroy = primary_generic_destroy,
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
    Surface* focus = mgr->seat->keyboard_focus();
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
    .destroy = primary_generic_destroy,
};
void primary_source_resource_destroy(wl_resource* resource) {
    PrimarySource* source = primary_source_of(resource);
    for (wl_resource* offer : source->offers) {
        primary_offer_of(offer)->source = nullptr;
    }
    if (source->mgr != nullptr && source->mgr->selection == source) {
        source->mgr->selection = nullptr;
        primary_broadcast(source->mgr);
    }
    delete source;
}

void primary_device_set_selection(wl_client*, wl_resource* resource, wl_resource* source_resource,
                                  uint32_t /*serial*/) {
    auto* mgr = static_cast<PrimaryMgr*>(wl_resource_get_user_data(resource));
    PrimarySource* source =
        source_resource != nullptr ? primary_source_of(source_resource) : nullptr;
    if (mgr->selection == source) {
        return;
    }
    if (mgr->selection != nullptr) {
        zwp_primary_selection_source_v1_send_cancelled(mgr->selection->resource);
    }
    mgr->selection = source;
    primary_broadcast(mgr);
}
constexpr struct zwp_primary_selection_device_v1_interface primary_device_impl = {
    .set_selection = primary_device_set_selection,
    .destroy = primary_generic_destroy,
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
    Surface* focus = mgr->seat->keyboard_focus();
    if (focus != nullptr && wl_resource_get_client(focus->c_resource()) == client) {
        primary_send_selection_to_client(mgr, client);
    }
}

constexpr struct zwp_primary_selection_device_manager_v1_interface primary_manager_impl = {
    .create_source = primary_manager_create_source,
    .get_device = primary_manager_get_device,
    .destroy = primary_generic_destroy,
};

void primary_manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource =
        wl_resource_create(client, &zwp_primary_selection_device_manager_v1_interface,
                           static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &primary_manager_impl, data, nullptr);
}

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
    impl->global =
        wl_global_create(impl->display, &zwp_primary_selection_device_manager_v1_interface, 1,
                         impl.get(), primary_manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(zwp_primary_selection_device_manager_v1) failed");
    }
    Impl* raw = impl.get();
    impl->focus_conn = seat.keyboard_focus_changed().connect([raw](SeatKeyboardFocus& e) {
        if (e.surface != nullptr) {
            primary_send_selection_to_client(raw, wl_resource_get_client(e.surface->c_resource()));
        }
    });
    return PrimarySelectionManager{std::move(impl)};
}

} // namespace luminaria
