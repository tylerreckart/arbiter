#pragma once

#include "tui/history_sidebar.h"
#include "tui/opentui/c_api.h"

namespace arbiter::opentui {

void draw_history_sidebar(OpenTuiHandle frame,
                          const HistorySidebarSnapshot& snap,
                          const Rect& rect);

} // namespace arbiter::opentui
