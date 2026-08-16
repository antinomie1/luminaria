// luminaria/foreign_toplevel.cppm — zwlr_foreign_toplevel_management_v1.
//
// The protocol a taskbar, dock or window switcher speaks: it lists every window
// on the desktop with its title, app id, state and output, and asks to
// activate / minimize / maximize / close them.
//
// Unlike wlroots, nothing has to be published by hand. `track(shell)` mirrors an
// XdgShell: a window appears in the list when it maps, its title and state
// updates follow the toplevel's own signals, and it disappears when it unmaps or
// dies. What comes back is `request()` — a taskbar asking for something, which
// the compositor grants by calling the usual Toplevel methods (or ignores).
//
// Note that this protocol hands one client the ability to enumerate and control
// every other client's windows. It is meant for the desktop's own components; a
// compositor that sandboxes applications should not expose the global to them.

module;

#include "detail/wayland_fwd.h"

#include <cstdint>
#include <wayland-server-core.h>
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

export module luminaria.desktop:foreign_toplevel;

import std;

import luminaria;

export namespace luminaria {

/// A window-list client asked for something. Nothing happens until the
/// compositor acts on it — this is a request, not a command.
struct ForeignToplevelRequest {
    enum class Kind {
        maximize,
        unmaximize,
        minimize,
        unminimize,
        fullscreen,
        unfullscreen,
        activate,
        close,
    };
    Toplevel& toplevel;
    Kind kind;
    /// The wl_seat the client acted on (`activate` only), else null.
    wl_resource* seat = nullptr;
    /// The wl_output the client wants (`fullscreen` only, may be null for
    /// "compositor's choice").
    wl_resource* output = nullptr;
};

/// The zwlr_foreign_toplevel_manager_v1 global (version 3). Move-only;
/// pointer-stable state.
class ForeignToplevelManager {
public:
    [[nodiscard]] static Result<ForeignToplevelManager> create(Display& display);

    ~ForeignToplevelManager();
    ForeignToplevelManager(ForeignToplevelManager&&) noexcept;
    ForeignToplevelManager& operator=(ForeignToplevelManager&&) noexcept;
    ForeignToplevelManager(const ForeignToplevelManager&) = delete;
    ForeignToplevelManager& operator=(const ForeignToplevelManager&) = delete;

    /// Publish every window of `shell` to window-list clients, and keep the list
    /// current by itself. Call it once, right after creating the shell; `shell`
    /// must outlive this manager.
    void track(XdgShell& shell);

    /// Publish one window that `track()` cannot see (an Xwayland window, say).
    /// Same lifecycle rules: it appears on map and vanishes on unmap/destroy.
    void add(Toplevel& toplevel);

    /// Tell clients which output this window is on. A taskbar per monitor shows
    /// only its own windows, so without this every window shows up everywhere.
    /// Null removes the association.
    void set_output(Toplevel& toplevel, OutputGlobal* output);

    /// Fires for each client request.
    [[nodiscard]] Signal<ForeignToplevelRequest>& request() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit ForeignToplevelManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Implements zwlr_foreign_toplevel_manager_v1 (version 3).
//
// Same shape as ext-workspace: the window list lives once in Impl, and every
// bound manager gets its own handle objects addressing it. What is different is
// that the list maintains itself — each Entry subscribes to its Toplevel's
// signals, so a title change or a state change reaches every taskbar with no
// help from the compositor.

namespace luminaria {

struct ForeignToplevelManager::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    Signal<ForeignToplevelRequest> request;
    Signal<NewToplevel>::Connection new_toplevel_conn;

    /// One tracked window. Announced to clients only while it is mapped.
    struct Entry {
        Toplevel* toplevel = nullptr;
        OutputGlobal* output = nullptr;
        bool announced = false;
        Signal<ToplevelMap>::Connection map_conn;
        Signal<ToplevelUnmap>::Connection unmap_conn;
        Signal<ToplevelDestroy>::Connection destroy_conn;
        Signal<ToplevelIdentityChange>::Connection identity_conn;
        Signal<ToplevelStateChange>::Connection state_conn;
        Signal<ToplevelParentChange>::Connection parent_conn;
    };
    std::vector<std::unique_ptr<Entry>> entries;

    /// One bound zwlr_foreign_toplevel_manager_v1.
    struct Binding {
        wl_resource* resource = nullptr;
        Impl* impl = nullptr;
        std::map<Entry*, wl_resource*> handles;
    };
    std::vector<std::unique_ptr<Binding>> bindings;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }

    Entry* entry_for(const Toplevel* toplevel) {
        auto it = std::find_if(entries.begin(), entries.end(),
                               [toplevel](const std::unique_ptr<Entry>& e) {
                                   return e->toplevel == toplevel;
                               });
        return it == entries.end() ? nullptr : it->get();
    }

    Entry& track_toplevel(Toplevel& toplevel);
    void announce(Entry& entry);
    void withdraw(Entry& entry);
    void forget(Entry& entry);
    void send_handle(Binding& binding, Entry& entry);
    void send_details(Binding& binding, Entry& entry);
    void send_state(wl_resource* handle, const Entry& entry);
    void send_parent(Binding& binding, wl_resource* handle, const Entry& entry);
    void refresh(Entry& entry);
};

namespace {

using FtImpl = ForeignToplevelManager::Impl;
using FtEntry = FtImpl::Entry;
using FtBinding = FtImpl::Binding;

/// user_data of a handle resource. `entry` is nulled when the window goes away
/// while the client still holds the object, which the protocol allows.
struct Handle {
    FtImpl* impl = nullptr;
    FtBinding* binding = nullptr;
    FtEntry* entry = nullptr;
};

Handle* handle_of(wl_resource* resource) {
    return static_cast<Handle*>(wl_resource_get_user_data(resource));
}

void emit(wl_resource* resource, ForeignToplevelRequest::Kind kind, wl_resource* seat = nullptr,
          wl_resource* output = nullptr) {
    Handle* h = handle_of(resource);
    if (h->entry == nullptr || h->entry->toplevel == nullptr) {
        return; // the window is already gone; the client just hasn't noticed
    }
    ForeignToplevelRequest event{*h->entry->toplevel, kind, seat, output};
    h->impl->request.emit(event);
}

void handle_set_maximized(wl_client*, wl_resource* resource) {
    emit(resource, ForeignToplevelRequest::Kind::maximize);
}
void handle_unset_maximized(wl_client*, wl_resource* resource) {
    emit(resource, ForeignToplevelRequest::Kind::unmaximize);
}
void handle_set_minimized(wl_client*, wl_resource* resource) {
    emit(resource, ForeignToplevelRequest::Kind::minimize);
}
void handle_unset_minimized(wl_client*, wl_resource* resource) {
    emit(resource, ForeignToplevelRequest::Kind::unminimize);
}
void handle_activate(wl_client*, wl_resource* resource, wl_resource* seat) {
    emit(resource, ForeignToplevelRequest::Kind::activate, seat);
}
void handle_close(wl_client*, wl_resource* resource) {
    emit(resource, ForeignToplevelRequest::Kind::close);
}
// Where the taskbar's own button for this window is, so a minimize animation can
// fly into it. luminaria animates nothing, so this is validated and dropped.
void handle_set_rectangle(wl_client*, wl_resource* resource, wl_resource*, int32_t, int32_t,
                          int32_t width, int32_t height) {
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_ERROR_INVALID_RECTANGLE,
                               "set_rectangle: negative size");
    }
}
void handle_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void handle_set_fullscreen(wl_client*, wl_resource* resource, wl_resource* output) {
    emit(resource, ForeignToplevelRequest::Kind::fullscreen, nullptr, output);
}
void handle_unset_fullscreen(wl_client*, wl_resource* resource) {
    emit(resource, ForeignToplevelRequest::Kind::unfullscreen);
}

constexpr struct zwlr_foreign_toplevel_handle_v1_interface handle_impl = {
    .set_maximized = handle_set_maximized,
    .unset_maximized = handle_unset_maximized,
    .set_minimized = handle_set_minimized,
    .unset_minimized = handle_unset_minimized,
    .activate = handle_activate,
    .close = handle_close,
    .set_rectangle = handle_set_rectangle,
    .destroy = handle_destroy_request,
    .set_fullscreen = handle_set_fullscreen,
    .unset_fullscreen = handle_unset_fullscreen,
};

void handle_resource_destroy(wl_resource* resource) {
    Handle* h = handle_of(resource);
    if (h->entry != nullptr) {
        h->binding->handles.erase(h->entry);
    }
    delete h;
}

void manager_stop(wl_client*, wl_resource* resource) {
    zwlr_foreign_toplevel_manager_v1_send_finished(resource);
    wl_resource_destroy(resource);
}

constexpr struct zwlr_foreign_toplevel_manager_v1_interface manager_impl = {
    .stop = manager_stop,
};

void manager_resource_destroy(wl_resource* resource) {
    auto* b = static_cast<FtBinding*>(wl_resource_get_user_data(resource));
    // Handles point back at this binding, so they cannot outlive it. Copy first:
    // each destroy erases its own entry from the map.
    std::vector<wl_resource*> handles;
    for (auto& [entry, r] : b->handles) {
        handles.push_back(r);
    }
    for (wl_resource* r : handles) {
        wl_resource_destroy(r);
    }
    std::erase_if(b->impl->bindings,
                  [b](const std::unique_ptr<FtBinding>& held) { return held.get() == b; });
}

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* impl = static_cast<FtImpl*>(data);
    wl_resource* resource = wl_resource_create(
        client, &zwlr_foreign_toplevel_manager_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto binding = std::make_unique<FtBinding>();
    binding->resource = resource;
    binding->impl = impl;
    FtBinding* b = binding.get();
    impl->bindings.push_back(std::move(binding));
    wl_resource_set_implementation(resource, &manager_impl, b, manager_resource_destroy);

    // A taskbar that starts late still gets the whole window list.
    for (auto& entry : impl->entries) {
        if (entry->announced) {
            impl->send_handle(*b, *entry);
        }
    }
}

} // namespace

FtEntry& FtImpl::track_toplevel(Toplevel& toplevel) {
    if (FtEntry* existing = entry_for(&toplevel); existing != nullptr) {
        return *existing;
    }
    auto owned = std::make_unique<FtEntry>();
    FtEntry& entry = *owned;
    entry.toplevel = &toplevel;
    entries.push_back(std::move(owned));

    entry.map_conn = toplevel.map.connect([this, &entry](ToplevelMap&) { announce(entry); });
    entry.unmap_conn = toplevel.unmap.connect([this, &entry](ToplevelUnmap&) { withdraw(entry); });
    entry.destroy_conn =
        toplevel.destroy.connect([this, &entry](ToplevelDestroy&) { forget(entry); });
    entry.identity_conn =
        toplevel.identity_change.connect([this, &entry](ToplevelIdentityChange&) { refresh(entry); });
    entry.state_conn =
        toplevel.state_change.connect([this, &entry](ToplevelStateChange&) { refresh(entry); });
    entry.parent_conn =
        toplevel.parent_change.connect([this, &entry](ToplevelParentChange&) { refresh(entry); });

    if (toplevel.mapped()) {
        announce(entry);
    }
    return entry;
}

void FtImpl::announce(FtEntry& entry) {
    if (entry.announced) {
        return;
    }
    entry.announced = true;
    for (auto& b : bindings) {
        send_handle(*b, entry);
    }
}

void FtImpl::withdraw(FtEntry& entry) {
    if (!entry.announced) {
        return;
    }
    entry.announced = false;
    for (auto& b : bindings) {
        auto it = b->handles.find(&entry);
        if (it == b->handles.end()) {
            continue;
        }
        // The client owns the handle object and destroys it in its own time; it
        // just no longer refers to a window.
        handle_of(it->second)->entry = nullptr;
        zwlr_foreign_toplevel_handle_v1_send_closed(it->second);
        b->handles.erase(it);
    }
}

void FtImpl::forget(FtEntry& entry) {
    withdraw(entry);
    std::erase_if(entries, [&entry](const std::unique_ptr<FtEntry>& e) { return e.get() == &entry; });
}

void FtImpl::send_handle(FtBinding& binding, FtEntry& entry) {
    wl_client* client = wl_resource_get_client(binding.resource);
    wl_resource* handle = wl_resource_create(client,
                                             &zwlr_foreign_toplevel_handle_v1_interface,
                                             wl_resource_get_version(binding.resource), 0);
    if (handle == nullptr) {
        return;
    }
    wl_resource_set_implementation(handle, &handle_impl, new Handle{this, &binding, &entry},
                                   handle_resource_destroy);
    binding.handles[&entry] = handle;
    zwlr_foreign_toplevel_manager_v1_send_toplevel(binding.resource, handle);
    if (entry.output != nullptr) {
        // Only a wl_output this client actually bound can be named in an event.
        if (wl_resource* out = entry.output->resource_for(client); out != nullptr) {
            zwlr_foreign_toplevel_handle_v1_send_output_enter(handle, out);
        }
    }
    send_details(binding, entry);
}

/// Everything that can change over a window's life, ending in the `done` that
/// makes the batch atomic. output_enter/leave is NOT here: those are edges, and
/// re-sending an enter on every title change would have clients counting the
/// same output twice.
void FtImpl::send_details(FtBinding& binding, FtEntry& entry) {
    auto it = binding.handles.find(&entry);
    if (it == binding.handles.end() || entry.toplevel == nullptr) {
        return;
    }
    wl_resource* handle = it->second;
    zwlr_foreign_toplevel_handle_v1_send_title(handle, entry.toplevel->title().c_str());
    zwlr_foreign_toplevel_handle_v1_send_app_id(handle, entry.toplevel->app_id().c_str());
    send_state(handle, entry);
    send_parent(binding, handle, entry);
    zwlr_foreign_toplevel_handle_v1_send_done(handle);
}

void FtImpl::send_state(wl_resource* handle, const FtEntry& entry) {
    wl_array states;
    wl_array_init(&states);
    const auto push = [&states](std::uint32_t state) {
        if (auto* slot = static_cast<std::uint32_t*>(wl_array_add(&states, sizeof(std::uint32_t)));
            slot != nullptr) {
            *slot = state;
        }
    };
    if (entry.toplevel->is_maximized()) {
        push(ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED);
    }
    if (entry.toplevel->is_minimized()) {
        push(ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED);
    }
    if (entry.toplevel->is_activated()) {
        push(ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED);
    }
    if (entry.toplevel->is_fullscreen() &&
        wl_resource_get_version(handle) >=
            ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN_SINCE_VERSION) {
        push(ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN);
    }
    zwlr_foreign_toplevel_handle_v1_send_state(handle, &states);
    wl_array_release(&states);
}

void FtImpl::send_parent(FtBinding& binding, wl_resource* handle, const FtEntry& entry) {
    if (wl_resource_get_version(handle) <
        ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_PARENT_SINCE_VERSION) {
        return;
    }
    wl_resource* parent_handle = nullptr;
    if (FtEntry* parent = entry_for(entry.toplevel->parent()); parent != nullptr) {
        if (auto it = binding.handles.find(parent); it != binding.handles.end()) {
            parent_handle = it->second;
        }
    }
    zwlr_foreign_toplevel_handle_v1_send_parent(handle, parent_handle);
}

void FtImpl::refresh(FtEntry& entry) {
    if (!entry.announced) {
        return;
    }
    for (auto& b : bindings) {
        send_details(*b, entry);
    }
}

ForeignToplevelManager::ForeignToplevelManager(std::unique_ptr<FtImpl> impl) noexcept
    : impl_(std::move(impl)) {}
ForeignToplevelManager::~ForeignToplevelManager() = default;
ForeignToplevelManager::ForeignToplevelManager(ForeignToplevelManager&&) noexcept = default;
ForeignToplevelManager& ForeignToplevelManager::operator=(ForeignToplevelManager&&) noexcept =
    default;

Result<ForeignToplevelManager> ForeignToplevelManager::create(Display& display) {
    auto impl = std::make_unique<FtImpl>();
    impl->display = display.c_ptr();
    // Version 3: set_fullscreen (v2) and the parent event (v3).
    impl->global = wl_global_create(impl->display, &zwlr_foreign_toplevel_manager_v1_interface, 3,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(zwlr_foreign_toplevel_manager_v1) failed");
    }
    return ForeignToplevelManager{std::move(impl)};
}

void ForeignToplevelManager::track(XdgShell& shell) {
    FtImpl* impl = impl_.get();
    impl->new_toplevel_conn = shell.new_toplevel().connect(
        [impl](NewToplevel& event) { (void)impl->track_toplevel(event.toplevel); });
}

void ForeignToplevelManager::add(Toplevel& toplevel) {
    (void)impl_->track_toplevel(toplevel);
}

void ForeignToplevelManager::set_output(Toplevel& toplevel, OutputGlobal* output) {
    FtImpl::Entry* entry = impl_->entry_for(&toplevel);
    if (entry == nullptr || entry->output == output) {
        return;
    }
    OutputGlobal* previous = entry->output;
    entry->output = output;
    if (!entry->announced) {
        return;
    }
    for (auto& b : impl_->bindings) {
        auto it = b->handles.find(entry);
        if (it == b->handles.end()) {
            continue;
        }
        wl_client* client = wl_resource_get_client(it->second);
        if (previous != nullptr) {
            if (wl_resource* out = previous->resource_for(client); out != nullptr) {
                zwlr_foreign_toplevel_handle_v1_send_output_leave(it->second, out);
            }
        }
        if (output != nullptr) {
            if (wl_resource* out = output->resource_for(client); out != nullptr) {
                zwlr_foreign_toplevel_handle_v1_send_output_enter(it->second, out);
            }
        }
        zwlr_foreign_toplevel_handle_v1_send_done(it->second);
    }
}

Signal<ForeignToplevelRequest>& ForeignToplevelManager::request() noexcept {
    return impl_->request;
}

} // namespace luminaria
