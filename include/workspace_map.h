#pragma once
// Cheap structural index of a host workspace for the /map writ.
// Tree listing only in this slice — no LSP / symbol outline.

#include <cstddef>
#include <string>
#include <string_view>

namespace arbiter {

struct WorkspaceMapOptions {
    // Max directory nesting under the map root (root itself is depth 0).
    int max_depth = 6;
    // Max files + directories emitted (truncation trailer when hit).
    int max_entries = 2000;
    // Soft cap on the returned body (bytes). 0 = unlimited.
    std::size_t max_bytes = 32 * 1024;
};

// Render a directory tree under `workspace_root` (must exist, or empty for
// process cwd via canonical_workspace_root).  Optional `rel_path` scopes
// the walk to a subdirectory; absolute / escaping paths return ERR.
// Output is an indented text tree suitable for a tool-result envelope.
std::string cmd_map(std::string_view workspace_root,
                    std::string_view rel_path = {},
                    const WorkspaceMapOptions& opts = {});

} // namespace arbiter
