#pragma once

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

void pane_history_init(Pane& pane);
void pane_history_set_cols(Pane& pane, int cols);
void pane_history_clear(Pane& pane);
void pane_history_push(Pane& pane, std::string_view text, bool new_block = false);
void pane_history_push_diff(Pane& pane, std::string_view patch);

void pane_history_push_prose(Pane& pane,
                             const std::vector<StyledLine>& lines,
                             bool new_block);
void pane_history_push_code_open(Pane& pane,
                                 std::string_view open_fence,
                                 size_t preview_rows,
                                 bool new_block = false);
void pane_history_push_code_line(Pane& pane, std::string_view line);
void pane_history_push_code_close(Pane& pane, std::string_view close_fence);
[[nodiscard]] int pane_history_total_rows(const Pane& pane);
[[nodiscard]] int pane_history_max_scroll(const Pane& pane);

void pane_history_begin_frame(UiContext& ctx);
void pane_history_draw_pane(Pane& pane, UiContext& ctx);
void pane_history_end_frame(UiContext& ctx);

// Convenience: single-pane immediate present.
void pane_history_render(Pane& pane, UiContext& ctx);

} // namespace arbiter
