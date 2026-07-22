#pragma once

#include "tui/opentui/c_api.h"
#include "tui/theme_picker.h"
#include "tui/tui.h"

namespace arbiter::opentui {

// Rows available for the theme list (excludes the header line).
[[nodiscard]] int theme_picker_visible_rows(const TUI& tui);

void draw_theme_picker(OpenTuiHandle frame,
                       const ThemePickerSnapshot& snap,
                       const TUI& tui);

} // namespace arbiter::opentui
