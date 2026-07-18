#pragma once

#include "tui/history_sidebar.h"
#include "tui/opentui/c_api.h"
#include "tui/tui.h"

namespace arbiter::opentui {

[[nodiscard]] int history_sidebar_visible_rows(const Rect& sidebar_rect,
                                               const Rect& pane_rect,
                                               int pane_input_rows,
                                               bool focused,
                                               int pane_bottom_pad_rows = TUI::kBottomPadRows);

void draw_history_sidebar(OpenTuiHandle frame,
                          const HistorySidebarSnapshot& snap,
                          const Rect& sidebar_rect,
                          const Rect& pane_rect,
                          int pane_input_rows,
                          int pane_bottom_pad_rows = TUI::kBottomPadRows);

} // namespace arbiter::opentui
