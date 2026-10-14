#pragma once

#include "tui/fleet.h"
#include "tui/opentui/c_api.h"
#include "tui/tui.h"

namespace arbiter::opentui {

// Tree rows that fit in `sidebar_rect` (below title + overlay, above
// the bottom border). The rect comes from fleet_sidebar_height; the
// pane geometry is not part of the budget.
[[nodiscard]] int fleet_sidebar_visible_rows(const Rect& sidebar_rect,
                                             int overlay_lines);

// Absolute y of the first tree row (matches the frame drawer).
[[nodiscard]] int fleet_sidebar_list_top(const Rect& sidebar_rect,
                                         int overlay_lines);

void draw_fleet_sidebar(OpenTuiHandle frame,
                        const FleetSnapshot& snap,
                        const Rect& sidebar_rect,
                        const Rect& pane_rect,
                        int pane_input_rows,
                        int pane_bottom_pad_rows = TUI::kBottomPadRows);

}  // namespace arbiter::opentui
