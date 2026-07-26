#pragma once
// arbiter/include/repl/layout_snapshot.h
//
// Serializable multi-pane layout: tree shape, split weights/orientation,
// and per-leaf conversation + agent identity.  Painted scrollback is not
// part of the snapshot — conversations reload their histories separately.
//
// On disk: ~/.arbiter/conversations/layout.json (next to manifest/active).

#include "repl/layout.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter {

// Hard cap matching the TUI's live pane limit (main.cpp kMaxPanes).
inline constexpr size_t kMaxLayoutSnapshotLeaves = 8;

struct LayoutSnapshot {
    struct Node {
        enum class Kind { Leaf, Split };

        Kind                 kind = Kind::Leaf;
        LayoutTree::Orient   orient = LayoutTree::Orient::Vertical;
        double               weight = 1.0;
        // Leaf fields (ignored for Split).
        std::string          conversation_id;
        std::string          agent = "index";
        // Split fields (empty for Leaf).
        std::vector<Node>    children;
    };

    static constexpr int kVersion = 1;

    int    version = kVersion;
    Node   root;
    // Pre-order leaf index of the focused pane.
    size_t focused_leaf = 0;
};

[[nodiscard]] size_t layout_snapshot_leaf_count(const LayoutSnapshot::Node& node);

// Structural checks only (version, tree shape, leaf cap, focused index).
// Does not verify that conversation ids still exist on disk.
[[nodiscard]] bool validate_layout_snapshot(const LayoutSnapshot& snap,
                                            size_t max_leaves = kMaxLayoutSnapshotLeaves);

[[nodiscard]] std::string layout_snapshot_to_json(const LayoutSnapshot& snap);
[[nodiscard]] std::optional<LayoutSnapshot>
layout_snapshot_from_json(std::string_view json);

[[nodiscard]] std::string layout_snapshot_path(const std::string& config_dir);

// Atomic write. Returns false on I/O failure or invalid snapshot.
bool save_layout_snapshot(const std::string& path, const LayoutSnapshot& snap);

// Missing/unreadable/invalid file → nullopt (caller keeps a single pane).
[[nodiscard]] std::optional<LayoutSnapshot>
load_layout_snapshot(const std::string& path);

// Visit every leaf in pre-order (mutable).
void for_each_layout_leaf(LayoutSnapshot::Node& node,
                          const std::function<void(LayoutSnapshot::Node&)>& fn);

}  // namespace arbiter
