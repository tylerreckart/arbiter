#pragma once

#include "tui/history_sidebar.h"
#include "tui/opentui/c_api.h"
#include "tui/tui.h"

namespace arbiter::opentui {

// `filter_line_visible` must match the frame: when true, the conversation
// list starts one row lower (after the "/" filter line).
// `pane_bottom_pad_rows` matches the focused pane's chrome budget so the
// list bottom aligns with scroll_bottom under chrome_compact_rows.
[[nodiscard]] int history_sidebar_visible_rows(const Rect& sidebar_rect,
                                               const Rect& pane_rect,
                                               int pane_input_rows,
                                               bool focused,
                                               bool filter_line_visible = false,
                                               int pane_bottom_pad_rows = TUI::kBottomPadRows);

void draw_history_sidebar(OpenTuiHandle frame,
                          const HistorySidebarSnapshot& snap,
                          const Rect& sidebar_rect,
                          const Rect& pane_rect,
                          int pane_input_rows,
                          int pane_bottom_pad_rows = TUI::kBottomPadRows);

} // namespace arbiter::opentui
