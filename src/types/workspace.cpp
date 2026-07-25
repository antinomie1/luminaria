module;

#include <cstdint>

// Implements ext_workspace_manager_v1 (version 1).
//
// Every bound manager gets its own handle objects for the same server-side
// workspaces, so the state lives once in Impl and each binding just holds the
// id -> wl_resource map needed to address it. Handles are destroyed with their
// binding, which is what keeps their back-pointers valid.

#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <wayland-server-core.h>

#include "ext-workspace-v1-protocol.h"

module luminaria;

namespace luminaria {

namespace {

struct GroupInfo {
    std::uint32_t id;
    std::vector<OutputGlobal*> outputs;
};

struct WorkspaceInfo {
    std::uint32_t id;
    std::uint32_t group;
    std::string str_id;
    std::string name;
    std::vector<std::int32_t> coordinates;
    std::uint32_t state = 0;
};

} // namespace

struct WorkspaceManager::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    Signal<WorkspaceRequest> request;
    std::vector<GroupInfo> groups;
    std::vector<WorkspaceInfo> workspaces;
    std::uint32_t next_id = 1;

    // One per bound ext_workspace_manager_v1.
    struct Binding {
        wl_resource* resource = nullptr;
        Impl* impl = nullptr;
        std::map<std::uint32_t, wl_resource*> group_handles;
        std::map<std::uint32_t, wl_resource*> workspace_handles;
        std::vector<WorkspaceRequest> queued;
    };
    std::vector<std::unique_ptr<Binding>> bindings;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }

    GroupInfo* group(std::uint32_t id) {
        auto it = std::find_if(groups.begin(), groups.end(),
                               [id](const GroupInfo& g) { return g.id == id; });
        return it == groups.end() ? nullptr : &*it;
    }
    WorkspaceInfo* workspace(std::uint32_t id) {
        auto it = std::find_if(workspaces.begin(), workspaces.end(),
                               [id](const WorkspaceInfo& w) { return w.id == id; });
        return it == workspaces.end() ? nullptr : &*it;
    }

    void send_group(Binding& b, const GroupInfo& g);
    void send_group_outputs(Binding& b, const GroupInfo& g);
    void send_workspace(Binding& b, const WorkspaceInfo& w);
    void send_done() {
        for (auto& b : bindings) {
            ext_workspace_manager_v1_send_done(b->resource);
        }
    }
};

namespace {

using Binding = WorkspaceManager::Impl::Binding;

// user_data of a group/workspace handle. Destroyed with the handle resource.
struct Handle {
    WorkspaceManager::Impl* impl;
    Binding* binding;
    std::uint32_t id;
    bool is_group;
};

Handle* handle_of(wl_resource* resource) {
    return static_cast<Handle*>(wl_resource_get_user_data(resource));
}

void handle_resource_destroy(wl_resource* resource) {
    Handle* h = handle_of(resource);
    auto& map = h->is_group ? h->binding->group_handles : h->binding->workspace_handles;
    map.erase(h->id);
    delete h;
}

void handle_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

// --- ext_workspace_handle_v1 -------------------------------------------------

void queue(wl_resource* resource, WorkspaceRequest::Kind kind, std::uint32_t group = 0,
           std::string name = {}) {
    Handle* h = handle_of(resource);
    h->binding->queued.push_back(
        WorkspaceRequest{kind, h->is_group ? 0u : h->id, group, std::move(name)});
}

void workspace_activate(wl_client*, wl_resource* resource) {
    queue(resource, WorkspaceRequest::Kind::activate);
}
void workspace_deactivate(wl_client*, wl_resource* resource) {
    queue(resource, WorkspaceRequest::Kind::deactivate);
}
void workspace_remove(wl_client*, wl_resource* resource) {
    queue(resource, WorkspaceRequest::Kind::remove);
}
void workspace_assign(wl_client*, wl_resource* resource, wl_resource* group_resource) {
    queue(resource, WorkspaceRequest::Kind::assign, handle_of(group_resource)->id);
}

constexpr struct ext_workspace_handle_v1_interface workspace_handle_impl = {
    .destroy = handle_destroy_request,
    .activate = workspace_activate,
    .deactivate = workspace_deactivate,
    .assign = workspace_assign,
    .remove = workspace_remove,
};

// --- ext_workspace_group_handle_v1 -------------------------------------------

void group_create_workspace(wl_client*, wl_resource* resource, const char* name) {
    Handle* h = handle_of(resource);
    h->binding->queued.push_back(WorkspaceRequest{WorkspaceRequest::Kind::create, 0, h->id,
                                                  name != nullptr ? name : ""});
}

constexpr struct ext_workspace_group_handle_v1_interface group_handle_impl = {
    .create_workspace = group_create_workspace,
    .destroy = handle_destroy_request,
};

// --- ext_workspace_manager_v1 ------------------------------------------------

void manager_commit(wl_client*, wl_resource* resource) {
    auto* b = static_cast<Binding*>(wl_resource_get_user_data(resource));
    std::vector<WorkspaceRequest> batch = std::move(b->queued);
    b->queued.clear();
    for (WorkspaceRequest& r : batch) {
        b->impl->request.emit(r);
    }
}

void manager_stop(wl_client*, wl_resource* resource) {
    ext_workspace_manager_v1_send_finished(resource);
    wl_resource_destroy(resource);
}

constexpr struct ext_workspace_manager_v1_interface manager_impl = {
    .commit = manager_commit,
    .stop = manager_stop,
};

void manager_resource_destroy(wl_resource* resource) {
    auto* b = static_cast<Binding*>(wl_resource_get_user_data(resource));
    // Handles hold a pointer back to this binding, so they die with it. Copy
    // first: each destroy erases its own entry from the map.
    std::vector<wl_resource*> handles;
    for (auto& [id, r] : b->group_handles) {
        handles.push_back(r);
    }
    for (auto& [id, r] : b->workspace_handles) {
        handles.push_back(r);
    }
    for (wl_resource* r : handles) {
        wl_resource_destroy(r);
    }
    std::erase_if(b->impl->bindings,
                  [b](const std::unique_ptr<Binding>& held) { return held.get() == b; });
}

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* impl = static_cast<WorkspaceManager::Impl*>(data);
    wl_resource* resource = wl_resource_create(client, &ext_workspace_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto binding = std::make_unique<Binding>();
    binding->resource = resource;
    binding->impl = impl;
    Binding* b = binding.get();
    impl->bindings.push_back(std::move(binding));
    wl_resource_set_implementation(resource, &manager_impl, b, manager_resource_destroy);

    for (const GroupInfo& g : impl->groups) {
        impl->send_group(*b, g);
    }
    for (const WorkspaceInfo& w : impl->workspaces) {
        impl->send_workspace(*b, w);
    }
    ext_workspace_manager_v1_send_done(resource);
}

} // namespace

void WorkspaceManager::Impl::send_group(Binding& b, const GroupInfo& g) {
    wl_client* client = wl_resource_get_client(b.resource);
    wl_resource* resource = wl_resource_create(client, &ext_workspace_group_handle_v1_interface,
                                               wl_resource_get_version(b.resource), 0);
    if (resource == nullptr) {
        return;
    }
    wl_resource_set_implementation(resource, &group_handle_impl, new Handle{this, &b, g.id, true},
                                   handle_resource_destroy);
    b.group_handles[g.id] = resource;
    ext_workspace_manager_v1_send_workspace_group(b.resource, resource);
    // We own the workspace set; clients may not add to it.
    ext_workspace_group_handle_v1_send_capabilities(resource, 0);
    send_group_outputs(b, g);
}

void WorkspaceManager::Impl::send_group_outputs(Binding& b, const GroupInfo& g) {
    auto it = b.group_handles.find(g.id);
    if (it == b.group_handles.end()) {
        return;
    }
    wl_client* client = wl_resource_get_client(b.resource);
    for (OutputGlobal* output : g.outputs) {
        // Only clients that actually bound the wl_output can be told about it.
        if (wl_resource* out = output->resource_for(client); out != nullptr) {
            ext_workspace_group_handle_v1_send_output_enter(it->second, out);
        }
    }
}

void WorkspaceManager::Impl::send_workspace(Binding& b, const WorkspaceInfo& w) {
    wl_client* client = wl_resource_get_client(b.resource);
    wl_resource* resource = wl_resource_create(client, &ext_workspace_handle_v1_interface,
                                               wl_resource_get_version(b.resource), 0);
    if (resource == nullptr) {
        return;
    }
    wl_resource_set_implementation(resource, &workspace_handle_impl,
                                   new Handle{this, &b, w.id, false}, handle_resource_destroy);
    b.workspace_handles[w.id] = resource;
    ext_workspace_manager_v1_send_workspace(b.resource, resource);
    ext_workspace_handle_v1_send_id(resource, w.str_id.c_str());
    ext_workspace_handle_v1_send_name(resource, w.name.c_str());

    wl_array coords;
    wl_array_init(&coords);
    for (std::int32_t c : w.coordinates) {
        if (void* slot = wl_array_add(&coords, sizeof(std::int32_t)); slot != nullptr) {
            *static_cast<std::int32_t*>(slot) = c;
        }
    }
    ext_workspace_handle_v1_send_coordinates(resource, &coords);
    wl_array_release(&coords);

    ext_workspace_handle_v1_send_state(resource, w.state);
    ext_workspace_handle_v1_send_capabilities(
        resource, EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE |
                      EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE);

    if (auto group = b.group_handles.find(w.group); group != b.group_handles.end()) {
        ext_workspace_group_handle_v1_send_workspace_enter(group->second, resource);
    }
}

WorkspaceManager::WorkspaceManager(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
WorkspaceManager::~WorkspaceManager() = default;
WorkspaceManager::WorkspaceManager(WorkspaceManager&&) noexcept = default;
WorkspaceManager& WorkspaceManager::operator=(WorkspaceManager&&) noexcept = default;

Signal<WorkspaceRequest>& WorkspaceManager::request() noexcept { return impl_->request; }

Result<WorkspaceManager> WorkspaceManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->global = wl_global_create(impl->display, &ext_workspace_manager_v1_interface, 1,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(ext_workspace_manager_v1) failed");
    }
    return WorkspaceManager{std::move(impl)};
}

std::uint32_t WorkspaceManager::add_group() {
    const std::uint32_t id = impl_->next_id++;
    impl_->groups.push_back(GroupInfo{id, {}});
    for (auto& b : impl_->bindings) {
        impl_->send_group(*b, impl_->groups.back());
    }
    return id;
}

void WorkspaceManager::group_add_output(std::uint32_t group, OutputGlobal& output) {
    GroupInfo* g = impl_->group(group);
    if (g == nullptr) {
        return;
    }
    g->outputs.push_back(&output);
    for (auto& b : impl_->bindings) {
        impl_->send_group_outputs(*b, *g);
    }
}

std::uint32_t WorkspaceManager::add_workspace(std::uint32_t group, std::string id, std::string name,
                                              std::vector<std::int32_t> coordinates) {
    const std::uint32_t handle = impl_->next_id++;
    impl_->workspaces.push_back(
        WorkspaceInfo{handle, group, std::move(id), std::move(name), std::move(coordinates), 0});
    for (auto& b : impl_->bindings) {
        impl_->send_workspace(*b, impl_->workspaces.back());
    }
    return handle;
}

void WorkspaceManager::set_state(std::uint32_t workspace, std::uint32_t state) {
    WorkspaceInfo* w = impl_->workspace(workspace);
    if (w == nullptr || w->state == state) {
        return;
    }
    w->state = state;
    for (auto& b : impl_->bindings) {
        if (auto it = b->workspace_handles.find(workspace); it != b->workspace_handles.end()) {
            ext_workspace_handle_v1_send_state(it->second, state);
        }
    }
}

void WorkspaceManager::activate(std::uint32_t workspace) {
    WorkspaceInfo* target = impl_->workspace(workspace);
    if (target == nullptr) {
        return;
    }
    // Exactly one active workspace per group is what every pager assumes.
    const std::uint32_t group = target->group;
    for (const WorkspaceInfo& w : impl_->workspaces) {
        if (w.group != group) {
            continue;
        }
        const bool active = w.id == workspace;
        set_state(w.id, active ? (w.state | workspace_active) : (w.state & ~workspace_active));
    }
}

void WorkspaceManager::remove_workspace(std::uint32_t workspace) {
    if (impl_->workspace(workspace) == nullptr) {
        return;
    }
    for (auto& b : impl_->bindings) {
        if (auto it = b->workspace_handles.find(workspace); it != b->workspace_handles.end()) {
            ext_workspace_handle_v1_send_removed(it->second);
        }
    }
    std::erase_if(impl_->workspaces,
                  [workspace](const WorkspaceInfo& w) { return w.id == workspace; });
}

void WorkspaceManager::done() { impl_->send_done(); }

} // namespace luminaria
