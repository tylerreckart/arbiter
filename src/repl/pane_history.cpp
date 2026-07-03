#include "repl/pane_history.h"

#include "repl/pane.h"
#include "tui/opentui/pane_frame.h"
#include "tui/opentui/pane_scroll_view.h"
#include "tui/opentui/session.h"

namespace arbiter {

void pane_history_init(Pane& pane) {
    pane.scroll = std::make_unique<opentui::PaneScrollView>();
    pane.scroll->bind(pane.tui);
}

void pane_history_set_cols(Pane& pane, int cols) {
    if (pane.scroll) pane.scroll->set_wrap_cols(cols);
}

void pane_history_clear(Pane& pane) {
    if (pane.scroll) pane.scroll->clear();
}

void pane_history_push(Pane& pane, std::string_view text) {
    if (pane.scroll) pane.scroll->append(text);
}

int pane_history_total_rows(const Pane& pane) {
    if (pane.scroll) return pane.scroll->total_visual_rows();
    return 0;
}

void pane_history_begin_frame(UiContext& ctx) {
    if (!ctx.session || !ctx.session->active()) return;
    ctx.session->begin_frame();
}

void pane_history_draw_pane(Pane& pane, UiContext& ctx) {
    if (!ctx.session || !pane.scroll) return;
    const OpenTuiHandle frame = ctx.session->frame();
    if (frame == 0) return;

    opentui::draw_pane_chrome(frame, pane.tui);
    pane.scroll->draw(frame,
                      pane.tui,
                      pane.scroll_offset,
                      pane.new_while_scrolled);
}

void pane_history_end_frame(UiContext& ctx) {
    if (!ctx.session) return;
    ctx.session->end_frame();
}

void pane_history_render(Pane& pane, UiContext& ctx) {
    pane_history_begin_frame(ctx);
    pane_history_draw_pane(pane, ctx);
    pane_history_end_frame(ctx);
}

} // namespace arbiter
