// luminaria/workspace.cppm — the ext_workspace_manager_v1 global.
//
// Workspaces belong to the compositor; this protocol only lets a panel or pager
// see them and ask for a switch. So the model here is server-owned: the
// compositor declares groups and workspaces, sets their state, and receives
// client requests as signals — it is never told what to do.
//
// Groups map to a set of outputs (one group per monitor is the usual layout);
// workspaces live in a group. Ids are opaque handles this class hands out.
//
// Importing luminaria pulls in no libwayland headers: the C types this
// interface names are forward-declared in the global module fragment.

module;

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

export module luminaria:workspace;

import :core.expected;
import :core.signal;

export namespace luminaria {

class Display;
class OutputGlobal;

/// ext_workspace_handle_v1 state bits.
enum WorkspaceState : std::uint32_t {
    workspace_active = 1,
    workspace_urgent = 2,
    workspace_hidden = 4,
};

/// A client asked for something. Nothing happens until the compositor acts:
/// switching workspaces is its decision, not the panel's.
struct WorkspaceRequest {
    enum class Kind { activate, deactivate, remove, assign, create };
    Kind kind;
    std::uint32_t workspace = 0; // 0 for `create`
    std::uint32_t group = 0;     // target group for `assign` / `create`
    std::string name;            // requested name for `create`
};

/// The ext_workspace_manager_v1 global (version 1). Move-only; pointer-stable
/// state so the libwayland global can hold a pointer to it.
class WorkspaceManager {
public:
    [[nodiscard]] static Result<WorkspaceManager> create(Display& display);

    ~WorkspaceManager();
    WorkspaceManager(WorkspaceManager&&) noexcept;
    WorkspaceManager& operator=(WorkspaceManager&&) noexcept;
    WorkspaceManager(const WorkspaceManager&) = delete;
    WorkspaceManager& operator=(const WorkspaceManager&) = delete;

    /// A new workspace group. Returns its handle id.
    [[nodiscard]] std::uint32_t add_group();
    /// Tell clients this group drives `output` (ext_workspace_group.output_enter).
    void group_add_output(std::uint32_t group, OutputGlobal& output);

    /// A new workspace in `group`. `id` is the stable string clients match on
    /// across compositor restarts; `coordinates` places it on a grid (e.g. {2}
    /// for the third workspace in a row), and may be empty.
    [[nodiscard]] std::uint32_t add_workspace(std::uint32_t group, std::string id,
                                              std::string name,
                                              std::vector<std::int32_t> coordinates = {});

    /// Replace a workspace's state bits (see WorkspaceState).
    void set_state(std::uint32_t workspace, std::uint32_t state);
    /// Make `workspace` the only active one in its group.
    void activate(std::uint32_t workspace);
    void remove_workspace(std::uint32_t workspace);

    /// Flush the changes made since the last call as one atomic update.
    /// Nothing above is visible to clients until this is called.
    void done();

    /// Fires for each client request, when the client commits its batch.
    [[nodiscard]] Signal<WorkspaceRequest>& request() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit WorkspaceManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
