#pragma once
// arbiter/include/repl/layout.h
//
// N-ary tree of Pane leaves with Split internal nodes.  Manages every Pane's
// rect and is the sole mutator of them.  All ops (resize/split/close/focus)
// go through LayoutTree so the invariant "every pane's tui.rect is valid"
// holds after any tree mutation.
//
// Structure:
//   • Leaf:   holds exactly one Pane; no children.
//   • Split:  holds ≥2 children laid out in a single orientation.
//       - Vertical   → children side-by-side; separator between each pair is
//                      a one-column │.
//       - Horizontal → children stacked top/bottom; separator is a one-row ─.
//   • Children of a split share the orientation dimension by weight
//     (default 1.0 each → equal split). Mouse drag on a separator adjusts
//     the two adjacent weights; keyboard asymmetric resize (#44) can reuse
//     the same weight field.
//
// Focus:
//   • Exactly one leaf is focused at any time.
//   • focus_next / focus_prev cycle leaves in pre-order.
//   • Closing the focused pane promotes its sibling subtree (when the
//     parent collapses to a single child) and focuses the nearest leaf.
//
// Splitting:
//   • Splitting the focused leaf with the same orientation as its parent
//     appends a new sibling to the parent — N panes now share 1/N each.
//   • Splitting with a different orientation (or when the focused leaf is
//     the root) wraps the focused leaf in a new 2-child split.
//
// Pane construction:
//   • The layout can't build Panes itself — Pane wiring needs app-scope
//     callbacks (orchestrator, cost tracker, loop manager, …).  split_focused
//     takes a factory function so callers can construct + wire panes before
//     handing them to the tree.

#include "repl/pane.h"
#include "tui/opentui/c_api.h"
#include "tui/tui.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace arbiter {

class LayoutTree {
public:
    enum class Orient { Vertical, Horizontal };
    using PaneFactory = std::function<std::unique_ptr<Pane>()>;

    // Public so layout.cpp's free-function helpers can name the type
    // without friend declarations.  Treat as opaque outside layout.cpp.
    struct Node {
        // Leaf:  pane non-null, children empty.
        // Split: pane null,     children.size() ≥ 2.
        std::unique_ptr<Pane>                   pane;
        std::vector<std::unique_ptr<Node>>      children;
        Orient                                   orient = Orient::Vertical;
        Rect                                     bounds;
        // Share of this node within its parent split. Ignored for the root.
        double                                   weight = 1.0;

        bool is_leaf() const { return pane != nullptr; }
    };

    // Stable separator identity: path of child indices from the root to the
    // split node, plus the separator's child index. Survives pointer
    // invalidation as long as the caller re-resolves under layout_mu; a
    // mutated tree simply fails resolve and the drag ends cleanly.
    struct SeparatorRef {
        std::vector<int> path;
        int              index = -1;  // child left/above the separator
        Orient           orient = Orient::Vertical;
    };

    // Build a tree with a single leaf covering `bounds`.  The pane's
    // tui.set_rect(bounds) is called immediately, so the caller only needs
    // to have entered alt-screen and initialized `first` before this.
    LayoutTree(std::unique_ptr<Pane> first, const Rect& bounds);
    ~LayoutTree();

    // Recompute every leaf's rect for a new outer bounds.  Chrome is repainted
    // by the output pump on the next frame.
    void resize(const Rect& bounds);

    [[nodiscard]] const Rect& outer_bounds() const { return bounds_; }

    // Paint split separators between sibling panes into an OpenTUI frame.
    void draw_borders(OpenTuiHandle frame) const;

    // Pre-order enumeration of leaves.
    void for_each_pane(const std::function<void(Pane&)>& fn);
    size_t pane_count() const;

    Pane& focused() const;
    Pane* focused_ptr() const { return focused_; }

    // Switch focus to the next/previous leaf in pre-order.  No-op if < 2.
    void focus_next();
    void focus_prev();

    // Focus a specific leaf.  No-op (returns false) if `pane` is not in the
    // tree. Used by click-to-focus mouse routing. Clears unfocused-activity
    // badges on the newly focused pane; exits zoom if focusing a sibling.
    bool focus_pane(Pane* pane);

    // Hit-test helpers. Coordinates are 0-based terminal cells.
    [[nodiscard]] Pane* pane_at(int x, int y) const;
    [[nodiscard]] std::optional<SeparatorRef> hit_separator(int x, int y);

    // Drag a separator so the cell under the pointer becomes the new
    // separator position. Adjusts the two adjacent children's weights and
    // recomputes bounds. Returns false if the ref is stale or clamping
    // refuses the move (min pane size).
    bool drag_separator(const SeparatorRef& sep, int pointer_x, int pointer_y);

    // Split the focused leaf and return a pointer to the newly created
    // pane (or nullptr if the split was refused — e.g. the focused leaf
    // is already at the minimum for the orientation).
    //
    // When `focus_new` is true (the default, matching tmux/vim Ctrl-w
    // semantics), focus moves to the new pane.  When false, focus stays
    // on the spawning pane — used by /pane so the user keeps typing in
    // the pane they invoked it from instead of being thrown into the
    // freshly created sibling.
    //
    // When the focused leaf's parent already splits along `orient`, the
    // new pane is inserted as a sibling right after it (so N panes share
    // 1/N of the parent's dimension, rebalanced on the next resize).
    // Otherwise the focused leaf is wrapped in a fresh 2-child split.
    Pane* split_focused(Orient orient, const PaneFactory& make_pane,
                         bool focus_new = true);

    // Remove the focused leaf.  Refuses to remove the last remaining pane.
    // If removal leaves the parent split with a single child, the parent
    // is collapsed (the remaining child is promoted to the parent's slot).
    // `on_destroy` is called on the pane about to be destroyed — callers
    // use it to stop its exec thread before the Pane destructor runs.
    bool close_focused(const std::function<void(Pane&)>& on_destroy = {});

    // Remove a specific pane by pointer.  Used by the delegation-chain
    // flow: when a spawned pane finishes and the user confirms close, the
    // pane being closed is often NOT the focused one.  Same rules as
    // close_focused otherwise — refuses if it's the last pane, collapses
    // single-child parent splits, and picks a new focused leaf if the
    // one being closed was focused.
    bool close_pane(Pane* target,
                    const std::function<void(Pane&)>& on_destroy = {});

    // Temporarily give the focused pane the full content rect (tmux zoom /
    // vim Ctrl-w _).  Second call restores the prior split arrangement.
    // Closing or splitting while zoomed clears zoom first.
    void toggle_zoom_focused();
    [[nodiscard]] bool zoomed() const { return zoomed_ != nullptr; }
    [[nodiscard]] Pane* zoomed_pane() const { return zoomed_; }
    void clear_zoom();

private:
    void compute_bounds(Node& n, const Rect& r);
    void collect_leaves(const Node& n, std::vector<Pane*>& out) const;
    void draw_borders_(const Node& n, OpenTuiHandle frame) const;
    void apply_chrome_flags();
    std::optional<SeparatorRef> hit_separator_(Node& n,
                                               int x,
                                               int y,
                                               std::vector<int>& path);
    [[nodiscard]] Node* resolve_split_(const SeparatorRef& sep);

    std::unique_ptr<Node> root_;
    Pane*                 focused_ = nullptr;
    Pane*                 zoomed_  = nullptr;  // nullptr = not zoomed
    Rect                  bounds_{};
};

} // namespace arbiter
