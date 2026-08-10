#pragma once

#include "tui/history_sidebar.h"
#include "tui/opentui/c_api.h"
#include "tui/tui.h"

namespace arbiter::opentui {

// `pane_bottom_pad_rows` matches the layout outer-bottom chrome budget so
// the list bottom stays aligned with column bottoms across focus changes.
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

// Floating action / move menu — paint after the modal scrim so the list
// behind it is dimmed with the rest of the TUI.
void draw_history_sidebar_menu(OpenTuiHandle frame,
                               const HistorySidebarSnapshot& snap,
                               const Rect& sidebar_rect,
                               const Rect& pane_rect,
                               int pane_input_rows,
                               int pane_bottom_pad_rows = TUI::kBottomPadRows);

// Name a new folder without replacing the "+ New" (or other) nav row.
void draw_history_new_folder_modal(OpenTuiHandle frame,
                                   const HistorySidebarSnapshot& snap,
                                   const TUI& tui);

} // namespace arbiter::opentui
