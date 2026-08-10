#pragma once

#include "tui/menu.h"
#include "tui/opentui/c_api.h"
#include "tui/tui.h"

namespace arbiter::opentui {

// Rows available for menu items (excludes border, title, and hint rows).
[[nodiscard]] int menu_visible_rows(const TUI& tui, int item_count);

// Floating modal above the focused pane's input strip.  Uses tui_menu_design()
// so it stays bright while modal dim recesses the rest of the TUI.
void draw_menu(OpenTuiHandle frame,
               const MenuSnapshot& snap,
               const TUI& tui);

// Anchored popover variant (conversation action menus).
void draw_menu_at(OpenTuiHandle frame,
                  const MenuSnapshot& snap,
                  int x,
                  int y,
                  int w,
                  int max_bottom_y);

} // namespace arbiter::opentui
