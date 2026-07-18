// arbiter/src/repl/layout.cpp — see repl/layout.h

#include "repl/layout.h"
#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <array>
#include <string>

namespace arbiter {

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

void LayoutTree::apply_chrome_flags() {
    std::vector<Pane*> leaves;
    collect_leaves(*root_, leaves);
    const bool multi = leaves.size() > 1 || zoomed_ != nullptr;
    for (auto* p : leaves) {
        // Zoomed: only the zoomed pane is visible → Full hint.
        // Multi: focused gets Compact chord hint; others Hidden (row reserved).
        // Single: Full.
        FooterHintMode mode = FooterHintMode::Full;
        if (zoomed_) {
            mode = (p == zoomed_) ? FooterHintMode::Full : FooterHintMode::Hidden;
        } else if (multi) {
            mode = (p == focused_) ? FooterHintMode::Compact : FooterHintMode::Hidden;
        }
        p->tui.set_footer_hint_mode(mode);
        p->tui.set_focus_accent(multi && p == focused_);
    }
}

void LayoutTree::resize(const Rect& bounds) {
    bounds_ = bounds;
    if (zoomed_ && root_) {
        // Zoom: focused (zoomed) pane gets the full outer rect; siblings keep
        // their tree slots but are assigned a degenerate off-screen rect so
        // they don't paint over the zoomed pane.
        std::vector<Pane*> leaves;
        collect_leaves(*root_, leaves);
        for (auto* p : leaves) {
            if (p == zoomed_) {
                p->tui.set_rect(bounds);
            } else {
                p->tui.set_rect(Rect{bounds.x, bounds.y, 0, 0});
            }
        }
        // Keep Node::bounds in sync for the zoomed leaf so draw_borders skips
        // (no visible separators while zoomed).
        root_->bounds = bounds;
    } else {
        compute_bounds(*root_, bounds);
    }
    apply_chrome_flags();
}

void LayoutTree::clear_zoom() {
    if (!zoomed_) return;
    zoomed_ = nullptr;
    resize(bounds_);
}

void LayoutTree::toggle_zoom_focused() {
    if (!focused_ || pane_count() < 2) return;
    if (zoomed_ == focused_) {
        clear_zoom();
        return;
    }
    zoomed_ = focused_;
    resize(bounds_);
}

void LayoutTree::draw_borders_(const Node& n, OpenTuiHandle frame) const {
    if (n.is_leaf() || frame == 0) return;

    const TuiDesign& d = tui_design();
    const TuiRgba& gutter = d.bg.scroll;

    for (int i = 0; i + 1 < static_cast<int>(n.children.size()); ++i) {
        const Rect& left = n.children[i]->bounds;
        const std::uint16_t* gutter_ptr = gutter.data();

        if (n.orient == Orient::Vertical) {
            const int sep_x = left.x + left.w;
            for (int y = n.bounds.y; y < n.bounds.y + n.bounds.h; ++y) {
                bufferFillRect(frame,
                               static_cast<std::uint32_t>(sep_x),
                               static_cast<std::uint32_t>(y),
                               1,
                               1,
                               gutter_ptr);
            }
        } else {
            const int sep_y = left.y + left.h;
            bufferFillRect(frame,
                           static_cast<std::uint32_t>(n.bounds.x),
                           static_cast<std::uint32_t>(sep_y),
                           static_cast<std::uint32_t>(n.bounds.w),
                           1,
                           gutter_ptr);
        }
    }

    for (const auto& child : n.children) draw_borders_(*child, frame);
}

void LayoutTree::draw_borders(OpenTuiHandle frame) const {
    if (!root_ || frame == 0 || zoomed_) return;
    draw_borders_(*root_, frame);
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
void clear_pane_activity(Pane* p) {
    if (!p) return;
    p->activity_unfocused.store(false);
    p->completed_unfocused.store(false);
    p->tui.clear_activity_badge();
}
} // namespace

bool LayoutTree::focus_pane(Pane* target) {
    if (!target) return false;
    std::vector<Pane*> leaves;
    collect_leaves(*root_, leaves);
    if (std::find(leaves.begin(), leaves.end(), target) == leaves.end()) return false;
    if (focused_ == target) return true;
    // Cycling focus while zoomed exits zoom so the newly focused pane is visible.
    if (zoomed_ && zoomed_ != target) clear_zoom();
    focused_ = target;
    clear_pane_activity(focused_);
    apply_chrome_flags();
    return true;
}

void LayoutTree::focus_next() {
    std::vector<Pane*> leaves;
    collect_leaves(*root_, leaves);
    if (leaves.size() < 2) return;
    auto it = std::find(leaves.begin(), leaves.end(), focused_);
    size_t idx = (it == leaves.end()) ? 0 : static_cast<size_t>(it - leaves.begin());
    focused_ = leaves[(idx + 1) % leaves.size()];
    if (zoomed_) {
        zoomed_ = focused_;
        resize(bounds_);
        clear_pane_activity(focused_);
        return;
    }
    clear_pane_activity(focused_);
    apply_chrome_flags();
}

void LayoutTree::focus_prev() {
    std::vector<Pane*> leaves;
    collect_leaves(*root_, leaves);
    if (leaves.size() < 2) return;
    auto it = std::find(leaves.begin(), leaves.end(), focused_);
    size_t idx = (it == leaves.end()) ? 0 : static_cast<size_t>(it - leaves.begin());
    focused_ = leaves[(idx + leaves.size() - 1) % leaves.size()];
    if (zoomed_) {
        zoomed_ = focused_;
        resize(bounds_);
        clear_pane_activity(focused_);
        return;
    }
    clear_pane_activity(focused_);
    apply_chrome_flags();
}

Pane* LayoutTree::split_focused(Orient orient, const PaneFactory& make_pane,
                                 bool focus_new) {
    if (!focused_) return nullptr;
    if (zoomed_) clear_zoom();
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

    if (zoomed_) clear_zoom();

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
        clear_pane_activity(focused_);
    }

    resize(bounds_);
    return true;
}

} // namespace arbiter
