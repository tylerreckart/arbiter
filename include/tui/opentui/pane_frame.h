#pragma once

#include "tui/opentui/c_api.h"
#include "tui/tui.h"

namespace arbiter::opentui {

// Draw pane chrome (header, separators, hints, input background) into an
// OpenTUI frame buffer.  Does not draw scrollback — PaneScrollView handles that.
void draw_pane_chrome(OpenTuiHandle frame, const TUI& tui);

} // namespace arbiter::opentui
