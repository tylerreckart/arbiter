#pragma once

#include "tui/fleet.h"
#include "tui/opentui/c_api.h"
#include "tui/tui.h"

namespace arbiter::opentui {

// Rows available for the tree list (below title + overlay).
[[nodiscard]] int fleet_sidebar_visible_rows(const Rect& sidebar_rect,
                                             const Rect& pane_rect,
                                             int pane_input_rows,
                                             int overlay_lines,
                                             int pane_bottom_pad_rows = TUI::kBottomPadRows);

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
