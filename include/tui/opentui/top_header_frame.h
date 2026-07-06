#pragma once

#include "tui/opentui/c_api.h"

namespace arbiter::opentui {

// Draws the single-row header spanning the full terminal width at row 0.
void draw_top_header(OpenTuiHandle frame, int cols);

} // namespace arbiter::opentui
