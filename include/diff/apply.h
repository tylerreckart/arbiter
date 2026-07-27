#pragma once
// arbiter/include/diff/apply.h
//
// First-class apply/undo for unified ```diff fences streamed into the TUI.
// Parses one-file unified diffs, validates paths stay under a workspace
// root (process cwd by default), applies with exact hunk-header matching
// (no whole-file context scan), and returns an undo snapshot so
// `/diff undo` can restore the pre-image when the file has not changed
// since apply.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter {

struct DiffHunkLine {
    enum class Tag : std::uint8_t { Context, Add, Remove };
    Tag         tag = Tag::Context;
    std::string text;   // without the leading +/-/space marker
};

struct DiffHunk {
    int old_start = 0;   // 1-based; 0 = empty (new file /dev/null)
    int old_count = 0;
    int new_start = 0;
    int new_count = 0;
    std::vector<DiffHunkLine> lines;
};

struct ParsedUnifiedDiff {
    std::string old_path;     // stripped of a/ prefix; empty if /dev/null
    std::string new_path;     // stripped of b/ prefix; empty if /dev/null
    bool        is_new_file = false;
    bool        is_delete   = false;
    std::vector<DiffHunk> hunks;
    std::string error;        // non-empty ⇒ parse failed
};

// Parse a single-file unified diff.  Multi-file patches, binary markers,
// and rename headers set `error` and leave hunks empty.
ParsedUnifiedDiff parse_unified_diff(std::string_view patch);

struct DiffApplyResult {
    bool        ok = false;
    std::string path;            // relative path written/deleted
    std::string resolved_path;   // absolute path under workspace
    std::string error;
    // Undo snapshot — valid when ok.
    bool        had_file = false;
    std::string pre_image;       // prior file bytes (empty if !had_file)
    std::string post_image;      // bytes after apply (empty if delete)
};

struct DiffUndoSnapshot {
    std::string resolved_path;
    bool        had_file = false;
    std::string pre_image;
    std::string post_image;
};

// Apply `patch` under `workspace_root` (must be an existing directory).
// Empty workspace_root ⇒ current_path().  Refuses absolute/escaping
// paths, stale context, and unsupported patch shapes.
DiffApplyResult apply_unified_diff(std::string_view patch,
                                   std::string_view workspace_root = {});

// Restore a prior DiffApplyResult snapshot.  Fails if the file no longer
// matches `post_image` (something else edited it after apply).
DiffApplyResult undo_unified_diff(const DiffUndoSnapshot& snap);

// Resolve a relative patch path against workspace_root with the same
// cwd-bound rules as /write.  Returns nullopt + err on escape/absolute.
std::optional<std::string>
resolve_workspace_path(std::string_view rel_path,
                       std::string_view workspace_root,
                       std::string& err);

} // namespace arbiter
