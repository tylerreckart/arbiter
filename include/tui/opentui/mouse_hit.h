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
    // Stable separator identity (path-based); valid only for SplitSeparator.
    LayoutTree::SeparatorRef sep{};
    // Absolute list-row index for HistorySidebar (0 = "+ New"), or -1 when
    // the click landed in sidebar chrome / empty list space.
    int history_row = -1;
};

inline bool rect_contains(const Rect& r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

// Map a 0-based y into a history-sidebar list row index, or -1 if outside
// the painted list band. `visible_rows` is the number of row slots currently
// drawn (from history_sidebar_visible_rows); clicks below that band return -1
// so empty chrome does not activate the last conversation.
inline int history_sidebar_row_at(const Rect& sidebar_rect,
                                  int y,
                                  int scroll_offset,
                                  int visible_rows) {
    // Matches history_sidebar_frame.cpp: list starts at sidebar_rect.y + 2.
    constexpr int kListTopOffset = 2;
    constexpr int kRowHeight = 2;
    const int top = sidebar_rect.y + kListTopOffset;
    if (y < top) return -1;
    if (visible_rows <= 0) return -1;
    const int rel = y - top;
    const int row_in_view = rel / kRowHeight;
    if (row_in_view < 0 || row_in_view >= visible_rows) return -1;
    if (rel >= visible_rows * kRowHeight) return -1;
    return scroll_offset + row_in_view;
}

// Classify which interactive region contains (x, y).
// `history_rect` / `right_rect` may be empty (w==0) when those sidebars are off.
// `history_visible_rows` clamps history list hits to painted rows only.
inline HitTarget hit_test(LayoutTree& layout,
                          const Rect& history_rect,
                          const Rect& right_rect,
                          int history_scroll_offset,
                          int history_visible_rows,
                          int x,
                          int y) {
    HitTarget hit;

    if (history_rect.w > 0 && rect_contains(history_rect, x, y)) {
        hit.kind = HitKind::HistorySidebar;
        hit.history_row = history_sidebar_row_at(
            history_rect, y, history_scroll_offset, history_visible_rows);
        return hit;
    }
    if (right_rect.w > 0 && rect_contains(right_rect, x, y)) {
        hit.kind = HitKind::RightSidebar;
        return hit;
    }

    if (auto sep = layout.hit_separator(x, y)) {
        hit.kind = HitKind::SplitSeparator;
        hit.sep = *sep;
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
