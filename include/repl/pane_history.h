#pragma once

#include "tui/tui.h"

#include <memory>
#include <string_view>

namespace arbiter::opentui {
class Session;
class PaneScrollView;
}

namespace arbiter {

struct Pane;

struct UiContext {
    opentui::Session* session = nullptr;
};

void pane_history_init(Pane& pane);
void pane_history_set_cols(Pane& pane, int cols);
void pane_history_clear(Pane& pane);
void pane_history_push(Pane& pane, std::string_view text);
[[nodiscard]] int pane_history_total_rows(const Pane& pane);

void pane_history_begin_frame(UiContext& ctx);
void pane_history_draw_pane(Pane& pane, UiContext& ctx);
void pane_history_end_frame(UiContext& ctx);

// Convenience: single-pane immediate present.
void pane_history_render(Pane& pane, UiContext& ctx);

} // namespace arbiter
