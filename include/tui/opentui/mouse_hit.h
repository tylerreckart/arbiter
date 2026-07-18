#pragma once
// Hit-testing helpers for mouse routing. Coordinates are 0-based cells
// (same space as Rect / OpenTUI bufferFillRect / decode_sgr_mouse).

#include "repl/layout.h"
#include "tui/history_sidebar.h"
#include "tui/opentui/mouse_decode.h"
#include "tui/tui.h"

#include <optional>

namespace arbiter::opentui {

enum class HitKind {
    Outside,
    HistorySidebar,
    RightSidebar,
    SplitSeparator,
    PaneScroll,
    PaneInput,
    PaneChrome,   // header / separators / hint — focus only
};

struct HitTarget {
    HitKind kind = HitKind::Outside;
    Pane*   pane = nullptr;
    // For SplitSeparator: parent split + index of the child left/above the sep.
    LayoutTree::Node* split = nullptr;
    int               sep_index = -1;
    LayoutTree::Orient sep_orient = LayoutTree::Orient::Vertical;
    // Absolute list-row index for HistorySidebar (0 = "+ New").
    int history_row = -1;
};

inline bool rect_contains(const Rect& r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

// Map a 0-based y into a history-sidebar list row index, or -1 if outside
// the list band. `scroll_offset` is the sidebar's current scroll.
inline int history_sidebar_row_at(const Rect& sidebar_rect,
                                  int y,
                                  int scroll_offset) {
    // Matches history_sidebar_frame.cpp: list starts at sidebar_rect.y + 2.
    constexpr int kListTopOffset = 2;
    constexpr int kRowHeight = 2;
    const int top = sidebar_rect.y + kListTopOffset;
    if (y < top) return -1;
    const int rel = y - top;
    return scroll_offset + rel / kRowHeight;
}

// Classify which interactive region contains (x, y).
// `history_rect` / `right_rect` may be empty (w==0) when those sidebars are off.
inline HitTarget hit_test(LayoutTree& layout,
                          const Rect& history_rect,
                          const Rect& right_rect,
                          int history_scroll_offset,
                          int x,
                          int y) {
    HitTarget hit;

    if (history_rect.w > 0 && rect_contains(history_rect, x, y)) {
        hit.kind = HitKind::HistorySidebar;
        hit.history_row = history_sidebar_row_at(history_rect, y, history_scroll_offset);
        return hit;
    }
    if (right_rect.w > 0 && rect_contains(right_rect, x, y)) {
        hit.kind = HitKind::RightSidebar;
        return hit;
    }

    if (auto sep = layout.hit_separator(x, y)) {
        hit.kind = HitKind::SplitSeparator;
        hit.split = sep->parent;
        hit.sep_index = sep->index;
        hit.sep_orient = sep->orient;
        return hit;
    }

    Pane* pane = layout.pane_at(x, y);
    if (!pane) {
        hit.kind = HitKind::Outside;
        return hit;
    }
    hit.pane = pane;

    // Match pane_frame.cpp geometry (0-based OpenTUI cells).
    const TuiChromeSnapshot chrome = pane->tui.chrome_snapshot();
    const Rect& r = chrome.rect;
    const int sep_y = r.y + r.h - TUI::kBottomPadRows - chrome.input_rows
                    - TUI::kSepRows;
    const int input_top = sep_y + 1;
    const int input_bottom = r.y + r.h - TUI::kBottomPadRows - 1;
    const int scroll_top = r.y;
    const int scroll_bottom = sep_y - 1;

    if (y >= input_top && y <= input_bottom
        && x >= r.x && x < r.x + r.w) {
        hit.kind = HitKind::PaneInput;
        return hit;
    }
    if (y >= scroll_top && y <= scroll_bottom
        && x >= r.x && x < r.x + r.w) {
        hit.kind = HitKind::PaneScroll;
        return hit;
    }
    hit.kind = HitKind::PaneChrome;
    return hit;
}

}  // namespace arbiter::opentui
