#include "repl/pane_history.h"

#include "repl/pane.h"
#include "tui/opentui/c_api.h"
#include "tui/opentui/pane_frame.h"
#include "tui/opentui/pane_scroll_view.h"
#include "tui/opentui/session.h"
#include "tui/tui_design.h"

#include <functional>

namespace arbiter {

void pane_history_drain_queue(Pane& pane) {
    auto items = pane.output_queue.drain_items();
    if (items.empty()) return;

    int before = (pane.scroll_offset > 0) ? pane_history_total_rows(pane) : 0;
    for (const auto& item : items) {
        switch (item.kind) {
        case OutputItem::Kind::Text:
            if (!item.data.empty()) {
                pane_history_push(pane, item.data, item.new_block);
            }
            break;
        case OutputItem::Kind::Prose:
            if (!item.styled_lines.empty()) {
                pane_history_push_prose(pane, item.styled_lines, item.new_block);
            }
            break;
        case OutputItem::Kind::Code:
            if (item.code_op == OutputItem::CodeOp::Open) {
                pane_history_push_code_open(
                    pane, item.data, item.code_lang, item.code_preview_rows, item.new_block);
            } else if (item.code_op == OutputItem::CodeOp::Line) {
                pane_history_push_code_line(pane, item.data);
            } else {
                pane_history_push_code_close(pane, item.data);
            }
            break;
        case OutputItem::Kind::Diff:
            pane_history_push_diff(pane, item.data);
            break;
        }
    }
    if (pane.scroll_offset > 0) {
        int after = pane_history_total_rows(pane);
        pane.new_while_scrolled += (after - before);
    }
}

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

void pane_history_retheme(Pane& pane) {
    if (pane.scroll) pane.scroll->retheme();
}

void pane_history_push(Pane& pane, std::string_view text, bool new_block) {
    if (pane.scroll) pane.scroll->append(text, new_block);
}

void pane_history_push_diff(Pane& pane, std::string_view patch) {
    if (pane.scroll) pane.scroll->append_diff(patch);
}

void pane_history_push_prose(Pane& pane,
                             const std::vector<StyledLine>& lines,
                             bool new_block) {
    if (pane.scroll) pane.scroll->append_prose(lines, new_block);
}

void pane_history_push_code_open(Pane& pane,
                                 std::string_view open_fence,
                                 std::string_view lang,
                                 size_t preview_rows,
                                 bool new_block) {
    if (pane.scroll) {
        pane.scroll->append_code_open(open_fence, lang, preview_rows, new_block);
    }
}

void pane_history_push_code_line(Pane& pane, std::string_view line) {
    if (pane.scroll) pane.scroll->append_code_line(line);
}

void pane_history_push_code_close(Pane& pane, std::string_view close_fence) {
    if (pane.scroll) pane.scroll->append_code_close(close_fence);
}

bool pane_history_toggle_code_block(Pane& pane, int scroll_offset) {
    if (!pane.scroll) return false;
    return pane.scroll->toggle_code_block_in_view(scroll_offset);
}

int pane_history_total_rows(const Pane& pane) {
    if (pane.scroll) return pane.scroll->total_visual_rows();
    return 0;
}

int pane_history_max_scroll(const Pane& pane) {
    if (pane.scroll) return pane.scroll->max_scroll_offset();
    return 0;
}

void pane_history_begin_frame(UiContext& ctx) {
    // Legacy entry point — prefer pane_history_present / Session::with_frame.
    if (!ctx.session || !ctx.session->active()) return;
    const OpenTuiHandle frame = ctx.session->begin_frame();
    if (frame == 0) return;
    bufferClear(frame, tui_design().bg.base.data());
}

void pane_history_draw_pane(Pane& pane, UiContext& ctx, OpenTuiHandle frame) {
    if (!ctx.session || !pane.scroll || frame == 0) return;

    opentui::draw_pane_chrome(frame, pane.tui);
    pane.scroll->draw(frame,
                      pane.tui,
                      pane.scroll_offset,
                      pane.new_while_scrolled);
    const bool focused = (&pane == ctx.focused_pane);
    pane.editor.draw(frame, pane.tui, focused);
}

void pane_history_end_frame(UiContext& ctx) {
    if (!ctx.session) return;
    ctx.session->end_frame();
}

void pane_history_present(UiContext& ctx, const PaneFrameHooks& hooks) {
    if (!ctx.session || !ctx.session->active()) return;
    ctx.session->with_frame([&](OpenTuiHandle frame) {
        if (frame == 0) return;
        bufferClear(frame, tui_design().bg.base.data());

        if (hooks.for_each_pane) {
            hooks.for_each_pane([](Pane& p) {
                p.thinking.tick();
                p.tool_indicator.tick();
            });
            hooks.for_each_pane([&](Pane& p) {
                pane_history_draw_pane(p, ctx, frame);
            });
        }
        if (hooks.draw_overlays) {
            hooks.draw_overlays(frame,
                                static_cast<int>(getBufferWidth(frame)),
                                static_cast<int>(getBufferHeight(frame)));
        }
    });
}

void pane_history_render(Pane& pane, UiContext& ctx) {
    PaneFrameHooks hooks;
    hooks.for_each_pane = [&](const std::function<void(Pane&)>& fn) { fn(pane); };
    pane_history_present(ctx, hooks);
}

} // namespace arbiter
