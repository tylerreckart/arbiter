#pragma once

#include "tui/opentui/c_api.h"
#include "tui/sidebar.h"
#include "tui/tui.h"

namespace arbiter::opentui {

void draw_sidebar(OpenTuiHandle frame,
                  const SidebarSnapshot& snap,
                  const Rect& sidebar_rect,
                  const Rect& pane_rect,
                  int pane_input_rows);

} // namespace arbiter::opentui
