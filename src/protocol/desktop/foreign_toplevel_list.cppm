// luminaria/foreign_toplevel_list.cppm — ext_foreign_toplevel_list_v1.
//
// The read-only, tool-neutral window list: every mapped toplevel with a title,
// an app id and an *identifier* — an opaque token that is the same for every
// client and never reused. It is the half of single-window screen sharing that
// answers "which windows are there to pick from?"; the other half is
// `ScreencopyManager::enable_toplevel_capture()`, which turns one of these
// handles into a capture source (ext-foreign-toplevel-image-capture-source).
//
// Unlike wlr-foreign-toplevel-management this protocol cannot *do* anything to
// a window — no activate, no close — which is why a portal or a sandboxed
// recorder is allowed to have it where a taskbar would get the wlr one. It
// still tells a client what the user has open, so it lives in the desktop
// module and is not a default global.
//
// Like ForeignToplevelManager, nothing is published by hand: `track(shell)`
// mirrors an XdgShell, a window appears when it maps, follows its own title /
// app_id signals, and is announced `closed` when it unmaps or dies.

module;

#include "detail/wayland_fwd.h"

#include <cstdint>
#include <wayland-server-core.h>
#include "ext-foreign-toplevel-list-v1-protocol.h"

export module luminaria.desktop:foreign_toplevel_list;

import std;

import luminaria;

export namespace luminaria {

/// The ext_foreign_toplevel_list_v1 global (version 1). Move-only;
/// pointer-stable state.
class ForeignToplevelList {
public:
    [[nodiscard]] static Result<ForeignToplevelList> create(Display& display);

    ~ForeignToplevelList();
    ForeignToplevelList(ForeignToplevelList&&) noexcept;
    ForeignToplevelList& operator=(ForeignToplevelList&&) noexcept;
    ForeignToplevelList(const ForeignToplevelList&) = delete;
    ForeignToplevelList& operator=(const ForeignToplevelList&) = delete;

    /// Publish every window of `shell` and keep the list current by itself.
    /// Call it once, right after creating the shell; `shell` must outlive this
    /// list.
    void track(XdgShell& shell);

    /// Publish one window `track()` cannot see. Same lifecycle rules.
    void add(Toplevel& toplevel);

    /// The identifier clients see for this window, or "" if it is not tracked.
    /// Opaque, unique for the life of the display, stable across a window's life.
    [[nodiscard]] std::string identifier(const Toplevel& toplevel) const;

    /// Which window an ext_foreign_toplevel_handle_v1 resource names, or null
    /// when the resource is not one of ours or the window has gone. This is the
    /// bridge to window capture: hand it to
    /// `ScreencopyManager::enable_toplevel_capture()`.
    [[nodiscard]] static Toplevel* toplevel_for(wl_resource* handle) noexcept;

    /// How many windows are currently announced. Mostly for tests.
    [[nodiscard]] std::size_t toplevel_count() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit ForeignToplevelList(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Implements ext_foreign_toplevel_list_v1 (version 1). Same shape as
// foreign_toplevel.cppm: the list lives once in Impl, every bound manager gets
// its own handle objects addressing it, and each Entry subscribes to its
// Toplevel's signals so a title change reaches every client with no help.

namespace luminaria {

struct ForeignToplevelList::Impl {
    WlGlobal global;
    Signal<NewToplevel>::Connection new_toplevel_conn;

    /// One tracked window. Announced to clients only while it is mapped.
    struct Entry {
        Toplevel* toplevel = nullptr;
        std::string identifier;
        std::string sent_title; // what clients last heard, so an unchanged
        std::string sent_app_id; // title does not wake every one of them
        bool announced = false;
        Signal<ToplevelMap>::Connection map_conn;
        Signal<ToplevelUnmap>::Connection unmap_conn;
        Signal<ToplevelDestroy>::Connection destroy_conn;
        Signal<ToplevelIdentityChange>::Connection identity_conn;
    };
    std::vector<std::unique_ptr<Entry>> entries;

    /// One bound ext_foreign_toplevel_list_v1.
    struct Binding {
        wl_resource* resource = nullptr;
        Impl* impl = nullptr;
        bool stopped = false;
        std::map<Entry*, wl_resource*> handles;
    };
    std::vector<std::unique_ptr<Binding>> bindings;

    Entry* entry_for(const Toplevel* toplevel) const {
        auto it = std::ranges::find_if(entries, [toplevel](const std::unique_ptr<Entry>& e) {
            return e->toplevel == toplevel;
        });
        return it == entries.end() ? nullptr : it->get();
    }

    Entry& track_toplevel(Toplevel& toplevel);
    void announce(Entry& entry);
    void withdraw(Entry& entry);
    void forget(Entry& entry);
    void send_handle(Binding& binding, Entry& entry);
    void refresh(Entry& entry);

    ~Impl();
};

namespace {

using FtlImpl = ForeignToplevelList::Impl;
using FtlEntry = FtlImpl::Entry;
using FtlBinding = FtlImpl::Binding;

/// user_data of a handle resource. `entry` is nulled when the window goes away
/// while the client still holds the object, which the protocol allows.
struct FtlHandle {
    FtlBinding* binding = nullptr;
    FtlEntry* entry = nullptr;
};

void handle_resource_destroy(wl_resource* resource) {
    auto* h = static_cast<FtlHandle*>(wl_resource_get_user_data(resource));
    if (h->entry != nullptr && h->binding != nullptr) {
        h->binding->handles.erase(h->entry);
    }
    delete h;
}

constexpr struct ext_foreign_toplevel_handle_v1_interface handle_impl = {
    .destroy = resource_destroy_request,
};

void manager_stop(wl_client*, wl_resource* resource) {
    auto* b = static_cast<FtlBinding*>(wl_resource_get_user_data(resource));
    if (b == nullptr || b->stopped) {
        return;
    }
    b->stopped = true;
    ext_foreign_toplevel_list_v1_send_finished(resource);
}

constexpr struct ext_foreign_toplevel_list_v1_interface manager_impl = {
    .stop = manager_stop,
    .destroy = resource_destroy_request,
};

void manager_resource_destroy(wl_resource* resource) {
    auto* b = static_cast<FtlBinding*>(wl_resource_get_user_data(resource));
    if (b == nullptr) {
        return; // the list is gone (see ~Impl)
    }
    // The client may keep handles past the manager — the protocol says they stay
    // valid — but nothing will ever be sent on them again. Sever them so they do
    // not point at a binding that is about to be freed.
    for (auto& [entry, r] : b->handles) {
        auto* h = static_cast<FtlHandle*>(wl_resource_get_user_data(r));
        h->binding = nullptr;
        h->entry = nullptr;
    }
    std::erase_if(b->impl->bindings,
                  [b](const std::unique_ptr<FtlBinding>& held) { return held.get() == b; });
}

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* impl = static_cast<FtlImpl*>(data);
    wl_resource* resource = wl_resource_create(
        client, &ext_foreign_toplevel_list_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto binding = std::make_unique<FtlBinding>();
    binding->resource = resource;
    binding->impl = impl;
    FtlBinding* b = binding.get();
    impl->bindings.push_back(std::move(binding));
    wl_resource_set_implementation(resource, &manager_impl, b, manager_resource_destroy);

    // A client that starts late still gets the whole list.
    for (auto& entry : impl->entries) {
        if (entry->announced) {
            impl->send_handle(*b, *entry);
        }
    }
}

/// 128 random bits, hex. The protocol wants the identifier unique for the life
/// of the compositor and unguessable enough that a client cannot enumerate
/// windows it was not told about; it also says nothing may be read into it.
std::string make_identifier() {
    static std::random_device device;
    static std::uint64_t counter = 0;
    // The counter is what guarantees uniqueness; the random half is what keeps
    // the sequence from being a list of small integers to guess.
    return std::format("{:016x}{:08x}{:08x}", ++counter, device(), device());
}

} // namespace

// The bindings die with us, but the manager and handle resources pointing at
// them are the client's and outlive us at exit (the Display, declared first, is
// what destroys a still-connected client). Leave them inert.
FtlImpl::~Impl() {
    for (auto& b : bindings) {
        for (auto& [entry, r] : b->handles) {
            auto* h = static_cast<FtlHandle*>(wl_resource_get_user_data(r));
            h->binding = nullptr;
            h->entry = nullptr;
        }
        detach_resource(b->resource);
    }
}

FtlEntry& FtlImpl::track_toplevel(Toplevel& toplevel) {
    if (FtlEntry* existing = entry_for(&toplevel); existing != nullptr) {
        return *existing;
    }
    auto owned = std::make_unique<FtlEntry>();
    FtlEntry& entry = *owned;
    entry.toplevel = &toplevel;
    entry.identifier = make_identifier();
    entries.push_back(std::move(owned));

    entry.map_conn = toplevel.map.connect([this, &entry](ToplevelMap&) { announce(entry); });
    entry.unmap_conn = toplevel.unmap.connect([this, &entry](ToplevelUnmap&) { withdraw(entry); });
    entry.destroy_conn =
        toplevel.destroy.connect([this, &entry](ToplevelDestroy&) { forget(entry); });
    entry.identity_conn =
        toplevel.identity_change.connect([this, &entry](ToplevelIdentityChange&) { refresh(entry); });

    if (toplevel.mapped()) {
        announce(entry);
    }
    return entry;
}

void FtlImpl::announce(FtlEntry& entry) {
    if (entry.announced) {
        return;
    }
    entry.announced = true;
    entry.sent_title = entry.toplevel->title();
    entry.sent_app_id = entry.toplevel->app_id();
    for (auto& b : bindings) {
        if (!b->stopped) {
            send_handle(*b, entry);
        }
    }
}

void FtlImpl::withdraw(FtlEntry& entry) {
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
        // just no longer names a window.
        auto* h = static_cast<FtlHandle*>(wl_resource_get_user_data(it->second));
        h->entry = nullptr;
        ext_foreign_toplevel_handle_v1_send_closed(it->second);
        b->handles.erase(it);
    }
}

void FtlImpl::forget(FtlEntry& entry) {
    withdraw(entry);
    std::erase_if(entries,
                  [&entry](const std::unique_ptr<FtlEntry>& e) { return e.get() == &entry; });
}

void FtlImpl::send_handle(FtlBinding& binding, FtlEntry& entry) {
    wl_client* client = wl_resource_get_client(binding.resource);
    wl_resource* handle = wl_resource_create(client, &ext_foreign_toplevel_handle_v1_interface,
                                             wl_resource_get_version(binding.resource), 0);
    if (handle == nullptr) {
        return;
    }
    wl_resource_set_implementation(handle, &handle_impl, new FtlHandle{&binding, &entry},
                                   handle_resource_destroy);
    binding.handles[&entry] = handle;
    ext_foreign_toplevel_list_v1_send_toplevel(binding.resource, handle);
    // identifier first and exactly once, then the rest, then `done`.
    ext_foreign_toplevel_handle_v1_send_identifier(handle, entry.identifier.c_str());
    ext_foreign_toplevel_handle_v1_send_title(handle, entry.sent_title.c_str());
    ext_foreign_toplevel_handle_v1_send_app_id(handle, entry.sent_app_id.c_str());
    ext_foreign_toplevel_handle_v1_send_done(handle);
}

void FtlImpl::refresh(FtlEntry& entry) {
    if (!entry.announced) {
        return;
    }
    const bool title = entry.sent_title != entry.toplevel->title();
    const bool app_id = entry.sent_app_id != entry.toplevel->app_id();
    if (!title && !app_id) {
        return;
    }
    entry.sent_title = entry.toplevel->title();
    entry.sent_app_id = entry.toplevel->app_id();
    for (auto& b : bindings) {
        auto it = b->handles.find(&entry);
        if (it == b->handles.end()) {
            continue;
        }
        if (title) {
            ext_foreign_toplevel_handle_v1_send_title(it->second, entry.sent_title.c_str());
        }
        if (app_id) {
            ext_foreign_toplevel_handle_v1_send_app_id(it->second, entry.sent_app_id.c_str());
        }
        ext_foreign_toplevel_handle_v1_send_done(it->second);
    }
}

ForeignToplevelList::ForeignToplevelList(std::unique_ptr<FtlImpl> impl) noexcept
    : impl_(std::move(impl)) {}
ForeignToplevelList::~ForeignToplevelList() = default;
ForeignToplevelList::ForeignToplevelList(ForeignToplevelList&&) noexcept = default;
ForeignToplevelList& ForeignToplevelList::operator=(ForeignToplevelList&&) noexcept = default;

Result<ForeignToplevelList> ForeignToplevelList::create(Display& display) {
    auto impl = std::make_unique<FtlImpl>();
    auto global = create_wl_global<&ext_foreign_toplevel_list_v1_interface, manager_bind>(
        display, 1, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return ForeignToplevelList{std::move(impl)};
}

void ForeignToplevelList::track(XdgShell& shell) {
    FtlImpl* impl = impl_.get();
    impl->new_toplevel_conn = shell.new_toplevel().connect(
        [impl](NewToplevel& event) { (void)impl->track_toplevel(event.toplevel); });
}

void ForeignToplevelList::add(Toplevel& toplevel) {
    (void)impl_->track_toplevel(toplevel);
}

std::string ForeignToplevelList::identifier(const Toplevel& toplevel) const {
    const FtlEntry* entry = impl_->entry_for(&toplevel);
    return entry != nullptr ? entry->identifier : std::string{};
}

Toplevel* ForeignToplevelList::toplevel_for(wl_resource* handle) noexcept {
    if (handle == nullptr ||
        !wl_resource_instance_of(handle, &ext_foreign_toplevel_handle_v1_interface, &handle_impl)) {
        return nullptr;
    }
    auto* h = static_cast<FtlHandle*>(wl_resource_get_user_data(handle));
    return h != nullptr && h->entry != nullptr ? h->entry->toplevel : nullptr;
}

std::size_t ForeignToplevelList::toplevel_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        impl_->entries, [](const std::unique_ptr<FtlEntry>& e) { return e->announced; }));
}

} // namespace luminaria
