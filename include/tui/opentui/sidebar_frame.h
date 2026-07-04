#pragma once

#include "tui/opentui/c_api.h"
#include "tui/sidebar.h"
#include "tui/tui.h"

namespace arbiter::opentui {

void draw_sidebar(OpenTuiHandle frame,
                  const SidebarSnapshot& snap,
                  const Rect& rect,
                  const TuiChromeSnapshot& chrome);

} // namespace arbiter::opentui
