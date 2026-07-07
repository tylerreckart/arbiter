#include "repl/transcript_replay.h"

#include "render_policy.h"
#include "repl/pane.h"
#include "repl/pane_history.h"
#include "stream_renderer.h"
#include "theme.h"

#include <algorithm>

namespace arbiter {

namespace {

void drain_into(opentui::PaneScrollView& view, OutputQueue& queue) {
    auto items = queue.drain_items();
    for (const auto& item : items) {
        switch (item.kind) {
        case OutputItem::Kind::Text:
            if (!item.data.empty()) view.append(item.data, item.new_block);
            break;
        case OutputItem::Kind::Prose:
            if (!item.styled_lines.empty()) view.append_prose(item.styled_lines, item.new_block);
            break;
        case OutputItem::Kind::Code:
            if (item.code_op == OutputItem::CodeOp::Open) {
                view.append_code_open(item.data, item.code_lang, item.code_preview_rows,
                                      item.new_block);
            } else if (item.code_op == OutputItem::CodeOp::Line) {
                view.append_code_line(item.data);
            } else {
                view.append_code_close(item.data);
            }
            break;
        case OutputItem::Kind::Diff:
            view.append_diff(item.data);
            break;
        }
    }
}

// Feeds messages [begin, end) through the same StreamRenderer pipeline a
// live turn uses, into `view` via `queue`. Shared by the initial tail
// replay and by loading an older chunk into a scratch view.
void render_messages(opentui::PaneScrollView& view,
                     OutputQueue& queue,
                     const std::vector<Message>& history,
                     std::size_t begin,
                     std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
        const Message& m = history[i];
        if (is_replay_noise(m)) continue;

        if (m.role == "user") {
            queue.push_msg(theme().user_echo_arrow + "> " + theme().user_echo_text
                          + m.content + theme().reset + "");
            continue;
        }

        StreamRenderer renderer(kReplay, queue);
        renderer.feed(m.content);
        renderer.flush();
        queue.end_message();
    }
    drain_into(view, queue);
}

} // namespace

void replay_transcript(Pane& pane,
                       const std::vector<Message>& history,
                       std::size_t begin,
                       std::size_t end) {
    end = std::min(end, history.size());
    if (begin > end) begin = end;
    if (!pane.scroll) return;

    render_messages(*pane.scroll, pane.output_queue, history, begin, end);
    pane.scroll->set_gap(static_cast<int>(begin));
}

bool replay_load_previous_chunk(Pane& pane, const std::vector<Message>& history) {
    if (!pane.scroll || !pane.scroll->has_gap()) return false;

    const std::size_t begin = static_cast<std::size_t>(pane.scroll->gap_remaining());
    if (begin == 0) {
        pane.scroll->set_gap(0);
        return false;
    }
    const std::size_t new_begin = (begin > kReplayChunkMessages) ? begin - kReplayChunkMessages : 0;

    opentui::PaneScrollView scratch;
    scratch.bind(pane.tui);
    OutputQueue scratch_queue;
    render_messages(scratch, scratch_queue, history, new_begin, begin);

    const int before = pane_history_total_rows(pane);
    pane.scroll->splice_front(scratch.take_segments());
    pane.scroll->set_gap(static_cast<int>(new_begin));
    const int after = pane_history_total_rows(pane);
    pane.scroll_offset += (after - before);
    return true;
}

} // namespace arbiter
