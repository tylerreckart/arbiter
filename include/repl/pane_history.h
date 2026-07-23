#pragma once

#include "commands.h"
#include "styled_text.h"
#include "tui/opentui/c_api.h"
#include "tui/tui.h"

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace arbiter::opentui {
class Session;
class PaneScrollView;
}

namespace arbiter {

struct Pane;

struct UiContext {
    opentui::Session* session = nullptr;
    Pane*             focused_pane = nullptr;
    // Repaint every pane in one OpenTUI frame (set once layout exists).
    std::function<void()> present_all;
};

struct PaneFrameHooks {
    std::function<void(const std::function<void(Pane&)>&)> for_each_pane;
    std::function<void(OpenTuiHandle frame, int cols, int rows)> draw_overlays;
};

void pane_history_present(UiContext& ctx, const PaneFrameHooks& hooks);

// Drains pane.output_queue into pane.scroll (the on-screen scrollback). The
// single consumer of OutputQueue's item stream — both live turns (via the
// getc pump) and transcript replay funnel through here so replayed output
// goes through the exact same code path as live output.
void pane_history_drain_queue(Pane& pane);

void pane_history_init(Pane& pane);
void pane_history_set_cols(Pane& pane, int cols);
void pane_history_clear(Pane& pane);
void pane_history_retheme(Pane& pane);
void pane_history_push(Pane& pane, std::string_view text, bool new_block = false);
void pane_history_push_diff(Pane& pane, std::string_view patch);

void pane_history_push_prose(Pane& pane,
                             const std::vector<StyledLine>& lines,
                             bool new_block);
void pane_history_push_code_open(Pane& pane,
                                 std::string_view open_fence,
                                 std::string_view lang,
                                 size_t preview_rows,
                                 bool new_block = false);
void pane_history_push_code_line(Pane& pane, std::string_view line);
void pane_history_push_code_close(Pane& pane, std::string_view close_fence);
void pane_history_upsert_tool(Pane& pane,
                              const ToolActivityEvent& event,
                              bool new_block = false);
bool pane_history_toggle_code_block(Pane& pane, int scroll_offset);
// Left-click expand/collapse for thinking / tool / truncated code blocks.
bool pane_history_toggle_expandable_at(Pane& pane, int term_x, int term_y);
[[nodiscard]] int pane_history_total_rows(const Pane& pane);
[[nodiscard]] int pane_history_max_scroll(const Pane& pane);

// /find support.  pane_history_find() records `term` on the pane and jumps
// to the last (most recent) match; pane_history_find_step() re-runs the
// recorded term and moves delta hits (+1 = older→newer, -1 = newer→older).
// Both return {hit_number (1-based), total_hits}; {0, 0} means no matches
// (or no recorded term for find_step).  Caller repositions are done via
// pane.scroll_offset, so hold layout_mu around these like any other
// scroll mutation.
struct PaneFindResult {
    int hit = 0;
    int total = 0;
};
PaneFindResult pane_history_find(Pane& pane, const std::string& term);
PaneFindResult pane_history_find_step(Pane& pane, int delta);

void pane_history_begin_frame(UiContext& ctx);
void pane_history_draw_pane(Pane& pane, UiContext& ctx, OpenTuiHandle frame);
void pane_history_end_frame(UiContext& ctx);

// Convenience: single-pane immediate present.
void pane_history_render(Pane& pane, UiContext& ctx);

} // namespace arbiter
