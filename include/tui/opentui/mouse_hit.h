#pragma once
// Hit-testing helpers for mouse routing. Coordinates are 0-based cells
// (same space as Rect / OpenTUI bufferFillRect / decode_sgr_mouse).

#include "repl/layout.h"
#include "tui/history_sidebar.h"
#include "tui/opentui/mouse_decode.h"
#include "tui/tui.h"

#include <optional>
#include <vector>

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
// the painted list band. `list_height_lines` is the vertical budget used by
// history_sidebar_visible_rows / the frame drawer. Row heights follow
// history_sidebar_row_height (1 for New/Section/Folder, 2 for Conversation).
inline int history_sidebar_row_at(const Rect& sidebar_rect,
                                  int y,
                                  int scroll_offset,
                                  int list_height_lines,
                                  const std::vector<HistorySidebarRow>& rows) {
    // Blank row above the box, title border, blank row inside, then list.
    // Keep in sync with history_sidebar_frame.cpp.
    const int top = sidebar_rect.y + 3;
    if (y < top || list_height_lines <= 0 || rows.empty()) return -1;
    const int band_end = top + list_height_lines;  // exclusive
    if (y >= band_end) return -1;

    int row_y = top;
    for (int i = scroll_offset; i < static_cast<int>(rows.size()); ++i) {
        const auto kind = rows[static_cast<size_t>(i)].kind;
        row_y += history_sidebar_gap_before(kind, i);
        const int h = history_sidebar_row_height(kind);
        // Frame skips any row that does not fully fit in the band.
        if (row_y + h > band_end) break;
        if (y >= row_y && y < row_y + h) return i;
        row_y += h;
    }
    return -1;
}

// Classify which interactive region contains (x, y).
// `history_rect` / `right_rect` may be empty (w==0) when those sidebars are off.
// `history_list_height` / `history_rows` clamp history list hits to painted,
// real rows only.
inline HitTarget hit_test(LayoutTree& layout,
                          const Rect& history_rect,
                          const Rect& right_rect,
                          int history_scroll_offset,
                          int history_list_height,
                          const std::vector<HistorySidebarRow>& history_rows,
                          int x,
                          int y) {
    HitTarget hit;

    if (history_rect.w > 0 && rect_contains(history_rect, x, y)) {
        hit.kind = HitKind::HistorySidebar;
        hit.history_row = history_sidebar_row_at(
            history_rect, y, history_scroll_offset, history_list_height,
            history_rows);
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
    const int bottom_pad = std::max(0, chrome.bottom_pad_rows);
    const int input_top = r.y + r.h - bottom_pad - chrome.input_rows;
    const int input_bottom = r.y + r.h - bottom_pad - 1;
    const int scroll_top = r.y;
    const int scroll_bottom = input_top - 1;

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
