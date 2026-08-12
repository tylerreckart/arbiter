#pragma once
// Shared host-filesystem workspace root resolution for /write and /diff apply.

#include <string>
#include <string_view>

namespace arbiter {

// Canonical absolute directory for host filesystem ops.
// - empty `root` → process cwd (legacy / CLI)
// - non-empty → must exist as a directory; otherwise returns "" and sets err
//   (no process-cwd fallback).  Rejects `session:` migration placeholders.
std::string canonical_workspace_root(std::string_view root,
                                     std::string* err = nullptr);

// True when `resolved` is exactly `root` or a strict child (accepts / and \).
bool path_within_canonical_root(std::string_view root,
                                std::string_view resolved);

// Agent ids become path components for memory scratchpads — reject traversal,
// absolute paths, hidden files, and non-alphanumeric characters.
bool agent_id_is_safe(const std::string& id);

} // namespace arbiter
