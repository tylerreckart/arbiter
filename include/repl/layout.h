#pragma once
// index/include/repl/layout.h
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
//   • All children of a split share the orientation dimension equally:
//     N children → each gets 1/N of (total − (N−1) separator cells), with
//     any rounding remainder spread across the leading children.
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
#include "tui/tui.h"

#include <functional>
#include <memory>
#include <vector>

namespace index_ai {

class LayoutTree {
public:
    enum class Orient { Vertical, Horizontal };
    using PaneFactory = std::function<std::unique_ptr<Pane>()>;

    // Build a tree with a single leaf covering `bounds`.  The pane's
    // tui.set_rect(bounds) is called immediately, so the caller only needs
    // to have entered alt-screen and initialized `first` before this.
    LayoutTree(std::unique_ptr<Pane> first, const Rect& bounds);
    ~LayoutTree();

    // Recompute every leaf's rect for a new outer bounds and repaint chrome.
    // Clears the terminal to avoid stale chrome from prior layout.
    void resize(const Rect& bounds);

    // Paint the separator characters between sibling panes.
    void render_borders();

    // Pre-order enumeration of leaves.
    void for_each_pane(const std::function<void(Pane&)>& fn);
    size_t pane_count() const;

    Pane& focused() const;
    Pane* focused_ptr() const { return focused_; }

    // Switch focus to the next/previous leaf in pre-order.  No-op if < 2.
    void focus_next();
    void focus_prev();

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

    // Public so layout.cpp's free-function helpers can name the type
    // without friend declarations.  Treat as opaque outside layout.cpp.
    struct Node {
        // Leaf:  pane non-null, children empty.
        // Split: pane null,     children.size() ≥ 2.
        std::unique_ptr<Pane>                   pane;
        std::vector<std::unique_ptr<Node>>      children;
        Orient                                   orient = Orient::Vertical;
        Rect                                     bounds;

        bool is_leaf() const { return pane != nullptr; }
    };

private:
    void compute_bounds(Node& n, const Rect& r);
    void collect_leaves(Node& n, std::vector<Pane*>& out) const;
    void render_borders_(Node& n);

    std::unique_ptr<Node> root_;
    Pane*                 focused_ = nullptr;
    Rect                  bounds_{};
};

} // namespace index_ai
