// ext-workspace-v1: a pager lists the compositor's workspaces, sees which one
// is active, and asks to switch. The switch is the compositor's to make.
#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "ext-workspace-v1-client-protocol.h"
#include "luminaria/core/display.hpp"
#include "luminaria/workspace.hpp"

namespace {

struct Seen {
    std::string id;
    std::string name;
    std::uint32_t state = 0;
};

struct ClientState {
    ext_workspace_manager_v1* manager = nullptr;
    std::vector<Seen> workspaces;
    std::vector<ext_workspace_handle_v1*> handles;
    int groups = 0;
    int dones = 0;
};

ClientState* g_client = nullptr;
Seen* seen_of(ext_workspace_handle_v1* h) {
    for (size_t i = 0; i < g_client->handles.size(); ++i) {
        if (g_client->handles[i] == h) {
            return &g_client->workspaces[i];
        }
    }
    return nullptr;
}

void ws_id(void*, ext_workspace_handle_v1* h, const char* id) { seen_of(h)->id = id; }
void ws_name(void*, ext_workspace_handle_v1* h, const char* name) { seen_of(h)->name = name; }
void ws_coordinates(void*, ext_workspace_handle_v1*, wl_array*) {}
void ws_state(void*, ext_workspace_handle_v1* h, uint32_t state) { seen_of(h)->state = state; }
void ws_capabilities(void*, ext_workspace_handle_v1*, uint32_t) {}
void ws_removed(void*, ext_workspace_handle_v1*) {}
const ext_workspace_handle_v1_listener kWorkspace{ws_id,           ws_name,    ws_coordinates,
                                                  ws_state,        ws_capabilities, ws_removed};

void grp_capabilities(void*, ext_workspace_group_handle_v1*, uint32_t) {}
void grp_output_enter(void*, ext_workspace_group_handle_v1*, wl_output*) {}
void grp_output_leave(void*, ext_workspace_group_handle_v1*, wl_output*) {}
void grp_workspace_enter(void*, ext_workspace_group_handle_v1*, ext_workspace_handle_v1*) {}
void grp_workspace_leave(void*, ext_workspace_group_handle_v1*, ext_workspace_handle_v1*) {}
void grp_removed(void*, ext_workspace_group_handle_v1*) {}
const ext_workspace_group_handle_v1_listener kGroup{
    grp_capabilities,     grp_output_enter,     grp_output_leave,
    grp_workspace_enter,  grp_workspace_leave,  grp_removed};

void mgr_workspace_group(void* data, ext_workspace_manager_v1*,
                         ext_workspace_group_handle_v1* group) {
    auto* st = static_cast<ClientState*>(data);
    ++st->groups;
    ext_workspace_group_handle_v1_add_listener(group, &kGroup, st);
}
void mgr_workspace(void* data, ext_workspace_manager_v1*, ext_workspace_handle_v1* workspace) {
    auto* st = static_cast<ClientState*>(data);
    st->handles.push_back(workspace);
    st->workspaces.emplace_back();
    ext_workspace_handle_v1_add_listener(workspace, &kWorkspace, st);
}
void mgr_done(void* data, ext_workspace_manager_v1*) {
    ++static_cast<ClientState*>(data)->dones;
}
void mgr_finished(void*, ext_workspace_manager_v1*) {}
const ext_workspace_manager_v1_listener kManager{mgr_workspace_group, mgr_workspace, mgr_done,
                                                 mgr_finished};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "ext_workspace_manager_v1") == 0) {
        st->manager = static_cast<ext_workspace_manager_v1*>(
            wl_registry_bind(reg, name, &ext_workspace_manager_v1_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState st;
    g_client = &st;
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistry, &st);
    wl_display_roundtrip(display);
    assert(st.manager != nullptr);
    ext_workspace_manager_v1_add_listener(st.manager, &kManager, &st);
    wl_display_roundtrip(display); // the initial dump lands here
    wl_display_roundtrip(display); // …plus the events it triggered

    assert(st.groups == 1);
    assert(st.workspaces.size() == 2);
    assert(st.workspaces[0].id == "ws-1");
    assert(st.workspaces[1].name == "two");
    assert(st.workspaces[0].state == luminaria::workspace_active);
    assert(st.workspaces[1].state == 0);

    // Ask for the second one. Nothing happens until commit.
    ext_workspace_handle_v1_activate(st.handles[1]);
    ext_workspace_manager_v1_commit(st.manager);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    assert(st.workspaces[0].state == 0);
    assert(st.workspaces[1].state == luminaria::workspace_active);
    wl_display_disconnect(display);
}

struct DestroyCtx {
    wl_listener listener;
    luminaria::Display* display;
};
void on_client_destroy(wl_listener* l, void*) {
    auto* ctx =
        reinterpret_cast<DestroyCtx*>(reinterpret_cast<char*>(l) - offsetof(DestroyCtx, listener));
    ctx->display->terminate();
}

int g_requests = 0;

} // namespace

int main() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto workspaces = luminaria::WorkspaceManager::create(*display);
    assert(workspaces.has_value());

    const std::uint32_t group = workspaces->add_group();
    const std::uint32_t first = workspaces->add_workspace(group, "ws-1", "one", {0});
    (void)workspaces->add_workspace(group, "ws-2", "two", {1});
    workspaces->activate(first);
    workspaces->done();

    auto conn = workspaces->request().connect([&](luminaria::WorkspaceRequest& r) {
        ++g_requests;
        assert(r.kind == luminaria::WorkspaceRequest::Kind::activate);
        workspaces->activate(r.workspace);
        workspaces->done();
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx dc{{}, &*display};
    dc.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &dc.listener);

    std::thread client_thread(run_client, fds[1]);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(3000);
    display->run();
    client_thread.join();

    assert(g_requests == 1);
    return 0;
}
