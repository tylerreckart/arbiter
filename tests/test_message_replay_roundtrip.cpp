#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "message_codec.h"
#include "repl/queues.h"
#include "repl/transcript_replay.h"
#include "render_policy.h"
#include "stream_renderer.h"
#include "styled_text.h"
#include "tui/opentui/pane_scroll_view.h"
#include "tui/tui.h"

using namespace arbiter;
using namespace arbiter::opentui;

namespace {

// Mirrors transcript_replay's render_messages drain path without pulling in
// Pane / PaneInputEditor (heavy OpenTUI editor deps).
void replay_into(PaneScrollView& view, const std::vector<Message>& history) {
    OutputQueue queue;
    for (const Message& m : history) {
        if (m.role == "user") {
            queue.push_prose(styled_user_echo_lines(replay_user_echo_text(m)));
            queue.end_message();
            continue;
        }
        if (!m.thinking.empty()) queue.push_thinking(m.thinking);
        StreamRenderer renderer(kReplay, queue);
        renderer.feed(m.content);
        renderer.flush();
        for (const auto& t : m.tool_trace) {
            ToolActivityEvent ev;
            ev.phase = ToolActivityEvent::Phase::Finished;
            ev.id = t.id;
            ev.label = t.label;
            ev.kind = t.kind;
            ev.detail = t.detail;
            ev.ok = t.ok;
            ev.result_preview = t.result_preview;
            queue.push_tool(ev);
        }
        queue.end_message();
    }
    for (const auto& item : queue.drain_items()) {
        switch (item.kind) {
        case OutputItem::Kind::Text:
            if (!item.data.empty()) view.append(item.data, item.new_block);
            break;
        case OutputItem::Kind::Prose:
            if (!item.styled_lines.empty())
                view.append_prose(item.styled_lines, item.new_block);
            break;
        case OutputItem::Kind::Code:
            if (item.code_op == OutputItem::CodeOp::Open) {
                view.append_code_open(item.data, item.code_lang,
                                      item.code_preview_rows, item.new_block);
            } else if (item.code_op == OutputItem::CodeOp::Line) {
                view.append_code_line(item.data);
            } else {
                view.append_code_close(item.data);
            }
            break;
        case OutputItem::Kind::Diff:
            view.append_diff(item.data);
            break;
        case OutputItem::Kind::Tool:
            view.upsert_tool(item.tool, item.new_block);
            break;
        case OutputItem::Kind::Thinking:
            if (!item.data.empty()) {
                view.append_thinking(item.data, item.new_block, item.agent_id);
            }
            break;
        }
    }
}

} // namespace

TEST_CASE("encode/decode messages preserves thinking and tool_trace") {
    Message user{"user", "do the thing"};
    Message asst{"assistant", "done with **bold**"};
    asst.thinking = "step one\nstep two";
    ToolTraceEntry t;
    t.id = "t42";
    t.label = "help";
    t.kind = "help";
    t.detail = "mem";
    t.ok = true;
    t.result_preview = "memory help";
    asst.tool_trace.push_back(t);

    const std::string json = encode_messages_json({user, asst});
    const auto back = decode_messages_json(json);
    REQUIRE(back.size() == 2);
    CHECK(back[0].role == "user");
    CHECK(back[0].content == "do the thing");
    CHECK(back[0].thinking.empty());
    CHECK(back[0].tool_trace.empty());

    CHECK(back[1].role == "assistant");
    CHECK(back[1].content == "done with **bold**");
    CHECK(back[1].thinking == "step one\nstep two");
    REQUIRE(back[1].tool_trace.size() == 1);
    CHECK(back[1].tool_trace[0].id == "t42");
    CHECK(back[1].tool_trace[0].label == "help");
    CHECK(back[1].tool_trace[0].ok);
    CHECK(back[1].tool_trace[0].result_preview == "memory help");
}

TEST_CASE("session json round-trip rebuilds thinking and tool rows in scroll") {
    Message user{"user", "investigate"};
    Message asst{"assistant", "here is the answer"};
    asst.thinking = "narrow the search space";
    ToolTraceEntry t;
    t.id = "t7";
    t.label = "fetch:https://example.com";
    t.kind = "fetch";
    t.detail = "https://example.com";
    t.ok = true;
    t.result_preview = "Example Domain";
    asst.tool_trace.push_back(t);

    const auto restored = decode_messages_json(
        encode_messages_json({user, asst}));

    TUI tui;
    tui.set_rect(Rect{0, 0, 80, 40});
    PaneScrollView view;
    view.bind(tui);
    replay_into(view, restored);

    const auto think_hits = view.find_rows("thinking");
    CHECK_FALSE(think_hits.empty());

    const auto tool_hits = view.find_rows("fetch:https://example.com");
    CHECK_FALSE(tool_hits.empty());

    const auto prose_hits = view.find_rows("here is the answer");
    CHECK_FALSE(prose_hits.empty());

    // Thinking appears before tool chrome in visual order.
    CHECK(think_hits.front() < tool_hits.front());
}
