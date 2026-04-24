// index/src/repl/layout.cpp — see repl/layout.h

#include "repl/layout.h"

#include <algorithm>
#include <cstdio>

namespace index_ai {

namespace {

// Find the Node slot (a unique_ptr reference) holding the leaf for
// `target_pane`.  Returns nullptr if not found.
std::unique_ptr<LayoutTree::Node>*
find_leaf_slot(std::unique_ptr<LayoutTree::Node>& slot,
               const Pane* target) {
    if (!slot) return nullptr;
    if (slot->is_leaf()) {
        return slot->pane.get() == target ? &slot : nullptr;
    }
    for (auto& child : slot->children) {
        if (auto* r = find_leaf_slot(child, target)) return r;
    }
    return nullptr;
}

// Find the parent Split node containing the leaf for `target_pane`, plus
// the child-index of the leaf within that parent.  Returns {nullptr, -1}
// when `target` is at the root (no parent).
struct ParentLoc {
    LayoutTree::Node* parent;   // null when target is root
    int               index;    // index into parent->children; -1 when parent null
};

ParentLoc find_parent(LayoutTree::Node& node, const Pane* target) {
    if (node.is_leaf()) return {nullptr, -1};
    for (int i = 0; i < (int)node.children.size(); ++i) {
        auto& child = node.children[i];
        if (child->is_leaf() && child->pane.get() == target) {
            return {&node, i};
        }
    }
    for (auto& child : node.children) {
        auto r = find_parent(*child, target);
        if (r.parent) return r;
    }
    return {nullptr, -1};
}

// Find the parent of a non-leaf `target_node`, and its index among that
// parent's children.  Used by close_focused to collapse a split that
// drops to a single child.  Returns {nullptr, -1} when `target_node` is
// the root.
struct ParentOfNode {
    LayoutTree::Node* parent;
    int               index;
};

ParentOfNode find_parent_of_node(LayoutTree::Node& node,
                                  const LayoutTree::Node* target) {
    if (node.is_leaf()) return {nullptr, -1};
    for (int i = 0; i < (int)node.children.size(); ++i) {
        if (node.children[i].get() == target) return {&node, i};
    }
    for (auto& child : node.children) {
        auto r = find_parent_of_node(*child, target);
        if (r.parent) return r;
    }
    return {nullptr, -1};
}

} // namespace

LayoutTree::LayoutTree(std::unique_ptr<Pane> first, const Rect& bounds) {
    root_ = std::make_unique<Node>();
    root_->pane = std::move(first);
    focused_ = root_->pane.get();
    bounds_ = bounds;
    compute_bounds(*root_, bounds);
    render_borders();
}

LayoutTree::~LayoutTree() = default;

void LayoutTree::compute_bounds(Node& n, const Rect& r) {
    n.bounds = r;
    if (n.is_leaf()) {
        n.pane->tui.set_rect(r);
        return;
    }
    // Split: divide evenly among all N children along the orientation axis,
    // with (N-1) cells reserved for separators between them.  If the
    // division doesn't land on integer boundaries, the leading children
    // absorb the +1 remainder so the total width/height adds up exactly.
    const int N = static_cast<int>(n.children.size());
    if (N < 1) return;
    const bool vertical = (n.orient == Orient::Vertical);
    const int total = vertical ? r.w : r.h;
    const int separators = std::max(0, N - 1);
    const int available = std::max(0, total - separators);
    const int base  = available / N;
    const int extra = available % N;

    int offset = 0;
    for (int i = 0; i < N; ++i) {
        int size = base + (i < extra ? 1 : 0);
        if (size < 1) size = 1;    // defensive: never emit a zero-width pane
        Rect child_rect;
        if (vertical) {
            child_rect = {r.x + offset, r.y, size, r.h};
        } else {
            child_rect = {r.x, r.y + offset, r.w, size};
        }
        compute_bounds(*n.children[i], child_rect);
        offset += size + 1;        // +1 for the separator cell
    }
}

void LayoutTree::resize(const Rect& bounds) {
    bounds_ = bounds;
    std::printf("\033[2J");          // clear once per relayout
    compute_bounds(*root_, bounds);

    // Footer hint is useful in single-pane mode and clutters every pane
    // when there are two or more.  Focus accent is only visible in
    // multi-pane mode — single-pane keeps the neutral dim-grey header
    // separator.  Both flags are resolved from the leaf count and the
    // current focused_ pointer; LayoutTree::focus_next/prev refresh the
    // focus accent independently when focus moves without a structural
    // change.
    std::vector<Pane*> leaves;
    collect_leaves(*root_, leaves);
    const bool multi = leaves.size() > 1;
    for (auto* p : leaves) {
        p->tui.set_footer_hint_visible(!multi);
        p->tui.set_focus_accent(multi && p == focused_);
        // Non-focused panes get a dim stub prompt so their input row
        // reads as an idle input surface.  The focused pane's input row
        // is left blank here; the REPL's begin_input re-paints the live
        // prompt via the refresh-pending handoff on the next main-loop
        // iteration (see cmd_interactive's refresh_pending flag).
        if (multi && p != focused_) p->tui.paint_idle_input_prompt();
    }

    render_borders();
}

void LayoutTree::render_borders_(Node& n) {
    if (n.is_leaf()) return;
    // Paint a separator between every adjacent pair of children.
    for (int i = 0; i + 1 < (int)n.children.size(); ++i) {
        const Rect& left = n.children[i]->bounds;
        if (n.orient == Orient::Vertical) {
            int sep_x = left.x + left.w;  // 0-indexed column of separator
            for (int y = n.bounds.y; y < n.bounds.y + n.bounds.h; ++y) {
                std::printf("\033[%d;%dH\033[38;5;237m│\033[0m",
                            y + 1, sep_x + 1);
            }
        } else {
            int sep_y = left.y + left.h;
            std::printf("\033[%d;%dH\033[38;5;237m",
                        sep_y + 1, n.bounds.x + 1);
            for (int k = 0; k < n.bounds.w; ++k) std::printf("─");
            std::printf("\033[0m");
        }
    }
    for (auto& child : n.children) render_borders_(*child);
}

void LayoutTree::render_borders() {
    render_borders_(*root_);
    std::fflush(stdout);
}

void LayoutTree::collect_leaves(Node& n, std::vector<Pane*>& out) const {
    if (n.is_leaf()) { out.push_back(n.pane.get()); return; }
    for (auto& child : n.children) collect_leaves(*child, out);
}

void LayoutTree::for_each_pane(const std::function<void(Pane&)>& fn) {
    std::vector<Pane*> leaves;
    collect_leaves(*root_, leaves);
    for (auto* p : leaves) fn(*p);
}

size_t LayoutTree::pane_count() const {
    std::vector<Pane*> leaves;
    collect_leaves(*root_, leaves);
    return leaves.size();
}

Pane& LayoutTree::focused() const {
    return *focused_;
}

namespace {
// Refresh focus-accent colors on every leaf and replace the leaving
// pane's live prompt with a dim idle stub.  Shared between focus_next /
// focus_prev so either direction behaves identically.  The incoming
// focused pane's LineEditor will repaint its live prompt on the next
// begin_input tick of the main loop.
void apply_focus_change(const std::vector<Pane*>& leaves,
                        Pane* old_focused, Pane* new_focused) {
    if (old_focused && old_focused != new_focused)
        old_focused->tui.paint_idle_input_prompt();
    const bool multi = leaves.size() > 1;
    for (auto* p : leaves) {
        p->tui.set_focus_accent(multi && p == new_focused);
    }
}
} // namespace

void LayoutTree::focus_next() {
    std::vector<Pane*> leaves;
    collect_leaves(*root_, leaves);
    if (leaves.size() < 2) return;
    auto it = std::find(leaves.begin(), leaves.end(), focused_);
    size_t idx = (it == leaves.end()) ? 0 : static_cast<size_t>(it - leaves.begin());
    Pane* old = focused_;
    focused_ = leaves[(idx + 1) % leaves.size()];
    apply_focus_change(leaves, old, focused_);
    render_borders();
}

void LayoutTree::focus_prev() {
    std::vector<Pane*> leaves;
    collect_leaves(*root_, leaves);
    if (leaves.size() < 2) return;
    auto it = std::find(leaves.begin(), leaves.end(), focused_);
    size_t idx = (it == leaves.end()) ? 0 : static_cast<size_t>(it - leaves.begin());
    Pane* old = focused_;
    focused_ = leaves[(idx + leaves.size() - 1) % leaves.size()];
    apply_focus_change(leaves, old, focused_);
    render_borders();
}

Pane* LayoutTree::split_focused(Orient orient, const PaneFactory& make_pane,
                                 bool focus_new) {
    if (!focused_) return nullptr;
    auto* slot = find_leaf_slot(root_, focused_);
    if (!slot) return nullptr;

    // Minimum pane size check.  For appending into an existing
    // same-orientation parent we check the parent's total dimension;
    // for wrapping we check the focused leaf's dimension.
    const Rect& focused_rect = (*slot)->bounds;
    constexpr int kMinW = 24;
    constexpr int kMinH = 8;

    auto parent_loc = find_parent(*root_, focused_);
    const bool append_to_parent =
        parent_loc.parent != nullptr && parent_loc.parent->orient == orient;

    if (append_to_parent) {
        const Rect& pr = parent_loc.parent->bounds;
        const int N = (int)parent_loc.parent->children.size();
        const int total = (orient == Orient::Vertical) ? pr.w : pr.h;
        const int min = (orient == Orient::Vertical) ? kMinW : kMinH;
        const int per = (total - N) / (N + 1);
        if (per < min) return nullptr;
    } else {
        if (orient == Orient::Vertical) {
            if (focused_rect.w < 2 * kMinW + 1) return nullptr;
        } else {
            if (focused_rect.h < 2 * kMinH + 1) return nullptr;
        }
    }

    auto new_pane = make_pane();
    if (!new_pane) return nullptr;

    auto new_leaf = std::make_unique<Node>();
    new_leaf->pane = std::move(new_pane);
    Pane* new_pane_ptr = new_leaf->pane.get();

    if (append_to_parent) {
        parent_loc.parent->children.insert(
            parent_loc.parent->children.begin() + parent_loc.index + 1,
            std::move(new_leaf));
    } else {
        auto old_leaf = std::move(*slot);
        auto split = std::make_unique<Node>();
        split->orient = orient;
        split->children.push_back(std::move(old_leaf));
        split->children.push_back(std::move(new_leaf));
        *slot = std::move(split);
    }

    if (focus_new) focused_ = new_pane_ptr;
    resize(bounds_);
    return new_pane_ptr;
}

bool LayoutTree::close_focused(const std::function<void(Pane&)>& on_destroy) {
    return close_pane(focused_, on_destroy);
}

bool LayoutTree::close_pane(Pane* target,
                             const std::function<void(Pane&)>& on_destroy) {
    if (!target) return false;
    if (pane_count() <= 1) return false;

    auto parent_loc = find_parent(*root_, target);
    if (!parent_loc.parent) return false;  // target is root but count>1 → impossible

    // Let the caller stop the exec thread before the Pane destructor runs.
    if (on_destroy) on_destroy(*target);

    const bool closed_focused = (target == focused_);

    // Remove the target leaf from its parent.
    parent_loc.parent->children.erase(
        parent_loc.parent->children.begin() + parent_loc.index);

    // If the parent is now a single-child split, collapse it.
    Node* parent = parent_loc.parent;
    if (parent->children.size() == 1) {
        auto gp_loc = find_parent_of_node(*root_, parent);
        if (!gp_loc.parent) {
            root_ = std::move(parent->children[0]);
        } else {
            gp_loc.parent->children[gp_loc.index] =
                std::move(parent->children[0]);
        }
    }

    // Pick a new focused leaf if the one we closed was focused; otherwise
    // keep the existing focus (it's still in the tree).
    if (closed_focused) {
        std::vector<Pane*> leaves;
        collect_leaves(*root_, leaves);
        focused_ = leaves.empty() ? nullptr : leaves.front();
    }

    resize(bounds_);
    return true;
}

} // namespace index_ai
