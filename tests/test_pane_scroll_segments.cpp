#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "commands.h"
#include "styled_text.h"
#include "tui/opentui/pane_scroll_view.h"
#include "tui/tui.h"
#include "tui/tui_design.h"

using namespace arbiter;
using namespace arbiter::opentui;

namespace {

void bind_view(PaneScrollView& view, TUI& tui, int cols = 80, int rows = 40) {
    tui.set_rect(Rect{0, 0, cols, rows});
    view.bind(tui);
}

} // namespace

TEST_CASE("ToolSegment Started then Finished updates one row") {
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui);

    ToolActivityEvent start;
    start.phase = ToolActivityEvent::Phase::Started;
    start.id = "t99";
    start.label = "help";
    start.kind = "help";
    start.detail = "mem";
    view.upsert_tool(start);

    const int after_start = view.total_visual_rows();
    CHECK(after_start >= 1);

    ToolActivityEvent done = start;
    done.phase = ToolActivityEvent::Phase::Finished;
    done.ok = true;
    done.result_preview = "ok body";
    view.upsert_tool(done);

    // Same id must update in place — not add a second row.
    CHECK(view.total_visual_rows() == after_start);

    // Expandable after detail/preview land.
    CHECK(view.toggle_code_block_in_view(/*scroll_offset=*/0));
    CHECK(view.total_visual_rows() > after_start);
}

TEST_CASE("ThinkingSegment honors wrap width and kPreviewRows when collapsed") {
    load_tui_design("");
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui, 40, 40);
    const int baseline = view.total_visual_rows();

    // Five short lines → top border + kPreviewRows (3, last with inline …)
    // + bottom border = 5 rows. Truncation no longer adds a dedicated row.
    view.append_thinking("one\ntwo\nthree\nfour\nfive");
    const int collapsed = view.total_visual_rows() - baseline;
    // May include a leading block_gap blank; the thinking box itself is 5 rows.
    CHECK(collapsed >= 5);
    CHECK(collapsed <= 7);

    CHECK(view.toggle_code_block_in_view(/*scroll_offset=*/0));
    const int expanded = view.total_visual_rows() - baseline;
    CHECK(expanded > collapsed);
    // Expanding reveals the two hidden body lines (no ellipsis-row swap).
    CHECK(expanded - collapsed == 2);
}

TEST_CASE("ThinkingSegment rounded box adds top and bottom border rows") {
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui, 80, 40);
    const int baseline = view.total_visual_rows();

    // One short line → top border + 1 body row + bottom border = 3 rows,
    // plus at most block_gap leading blanks.
    view.append_thinking("brief");
    const int rows = view.total_visual_rows() - baseline;
    CHECK(rows >= 3);
    CHECK(rows <= 4);
}

TEST_CASE("ThinkingSegment leaves block_gap after a tool row") {
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui, 80, 40);

    ToolActivityEvent done;
    done.phase = ToolActivityEvent::Phase::Finished;
    done.id = "t1";
    done.label = "mem:expand";
    done.kind = "mem";
    done.ok = true;
    view.upsert_tool(done);
    const int after_tool = view.total_visual_rows();
    CHECK(after_tool >= 1);

    // new_block=false mimics live tool→thinking (tools clear need_sep).
    view.append_thinking("brief reasoning", /*new_block=*/false);
    const int after_think = view.total_visual_rows();
    // tool (1) + block_gap (1) + box (top + body + bottom = 3) = 5.
    CHECK(after_think >= after_tool + 1 + 3);
}

TEST_CASE("ThinkingSegment streamed deltas grow one box, not stacked chrome") {
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui, 80, 40);

    view.append_thinking("one");
    const int after_first = view.total_visual_rows();

    // Contiguous delta appends into the same segment: exactly one more body
    // row; borders are counted once, so no duplicated chrome accumulates.
    view.append_thinking("\ntwo");
    CHECK(view.total_visual_rows() == after_first + 1);

    view.append_thinking("\nthree");
    CHECK(view.total_visual_rows() == after_first + 2);
}

TEST_CASE("ThinkingSegment find maps title to the top border row") {
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui, 80, 40);

    view.append_thinking("alpha\nbeta");
    const auto title_hits = view.find_rows("thinking");
    REQUIRE(title_hits.size() == 1);

    const auto body_hits = view.find_rows("beta");
    REQUIRE(body_hits.size() == 1);
    // Title sits on the top border; body rows start directly beneath it.
    CHECK(body_hits[0] == title_hits[0] + 2);
    CHECK(title_hits[0] + 2 < view.total_visual_rows());
}

TEST_CASE("ThinkingSegment renders markdown instead of raw asterisks") {
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui, 80, 40);

    view.append_thinking("**Correcting memory links**\n\nbody text");
    CHECK(view.find_rows("**Correcting").empty());
    CHECK_FALSE(view.find_rows("Correcting memory links").empty());
    CHECK_FALSE(view.find_rows("body text").empty());
}

TEST_CASE("ThinkingSegment folds truncation mark into the last preview row") {
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui, 40, 40);
    const int baseline = view.total_visual_rows();

    view.append_thinking("one\ntwo\nthree\nfour\nfive");
    const int collapsed = view.total_visual_rows() - baseline;
    // Box is top + 3 body + bottom (+ optional gap) — no lone ellipsis row.
    // gap(0|1) + 5 box rows.
    CHECK(collapsed >= 5);
    CHECK(collapsed <= 6);
    CHECK_FALSE(view.find_rows("three").empty());
    // Inline … is searchable as part of the last preview line.
    CHECK_FALSE(view.find_rows("\xE2\x80\xA6").empty());
}

TEST_CASE("ThinkingSegment wrap width grows visual rows for long lines") {
    load_tui_design("");
    // Long enough that even an 80-col wrap exceeds kPreviewRows (3).
    const std::string blob(400, 'x');

    TUI tui_n;
    PaneScrollView narrow;
    bind_view(narrow, tui_n, 20, 40);
    narrow.append_thinking(blob);

    TUI tui_w;
    PaneScrollView wide;
    bind_view(wide, tui_w, 80, 40);
    wide.append_thinking(blob);

    CHECK(narrow.total_visual_rows() >= wide.total_visual_rows());

    CHECK(narrow.toggle_code_block_in_view(0));
    CHECK(wide.toggle_code_block_in_view(0));
    CHECK(narrow.total_visual_rows() > wide.total_visual_rows());
}

TEST_CASE("ThinkingSegment renders markdown structure in body rows") {
    load_tui_design("");
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui, 80, 40);
    const int baseline = view.total_visual_rows();

    view.append_thinking("## Plan\n\n- step one\n- step two",
                         /*new_block=*/true,
                         "researcher");
    // pad + header + at least one body line + pad (+ optional gap).
    CHECK(view.total_visual_rows() - baseline >= 4);
}

TEST_CASE("block gaps are a single blank row between distinct segment kinds") {
    load_tui_design("");
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui, 80, 40);

    view.append_prose(styled_user_echo_lines("hello"));
    const int after_echo = view.total_visual_rows();
    // Lead-in blank under the header + top pad + text + bottom pad.
    CHECK(after_echo == 4);

    view.append_thinking("plan", /*new_block=*/true);
    const int after_think = view.total_visual_rows();
    // Echo bottom pad already supplies the one-row gap; thinking chrome is
    // pad + header + body + pad = 4 (no extra BlankSegment).
    CHECK(after_think - after_echo == 4);

    ToolActivityEvent start;
    start.phase = ToolActivityEvent::Phase::Started;
    start.id = "t1";
    start.label = "mem:search";
    start.kind = "mem";
    view.upsert_tool(start, /*new_block=*/false);

    ToolActivityEvent start2 = start;
    start2.id = "t2";
    start2.label = "mem:entry";
    view.upsert_tool(start2, /*new_block=*/false);

    // Thinking→tools: one BlankSegment; tools stay clustered.
    // Thinking's own bottom pad does not count as inter-block credit (only
    // prose/echo pads do), so tools still get an explicit gap.
    const int after_tools = view.total_visual_rows();
    CHECK(after_tools - after_think == 1 /*gap*/ + 2 /*tool rows*/);
}

TEST_CASE("trailing prose blanks do not stack with block_gap") {
    load_tui_design("");
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui, 80, 40);

    std::vector<StyledLine> prose;
    StyledLine body;
    styled_append(body, StyleId::Default, "paragraph");
    prose.push_back(body);
    prose.push_back(StyledLine{});  // soft trailing blanks
    prose.push_back(StyledLine{});
    view.append_prose(prose, /*new_block=*/true);
    const int after_prose = view.total_visual_rows();
    CHECK(after_prose == 3);  // lead-in blank + body + one soft blank

    view.append_thinking("next", /*new_block=*/true);
    const int after = view.total_visual_rows();
    // Soft blank is credited as the gap; thinking = pad+header+body+pad = 4.
    // Net: keep 1 soft blank as gap credit + 4 thinking rows beyond content
    // body (soft blank already in after_prose), so delta == 4.
    CHECK(after - after_prose == 4);
}

TEST_CASE("prose to tool gap is exactly one blank with no trailing phantom") {
    load_tui_design("");
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui, 80, 40);

    StyledLine line;
    styled_append(line, StyleId::Default, "answer");
    view.append_prose({line}, /*new_block=*/true);
    const int after_prose = view.total_visual_rows();
    CHECK(after_prose == 2);  // lead-in blank + body

    ToolActivityEvent start;
    start.phase = ToolActivityEvent::Phase::Started;
    start.id = "t1";
    start.label = "fetch:https://x";
    start.kind = "fetch";
    view.upsert_tool(start, /*new_block=*/false);
    CHECK(view.total_visual_rows() - after_prose == 1 /*gap*/ + 1 /*tool*/);
}

TEST_CASE("first user echo has a lead-in blank under the header") {
    load_tui_design("");
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui, 80, 40);
    CHECK(view.total_visual_rows() == 0);

    view.append_prose(styled_user_echo_lines("hi"), /*new_block=*/true);
    // BlankSegment + echo top pad + text + bottom pad.
    CHECK(view.total_visual_rows() == 4);
}

TEST_CASE("degenerate zero-size pane draw is a no-op") {
    // Zoom siblings / squeezed splits get w==0 (and often h==0). Painting
    // those used to reach OpenTUI with negative origins cast to uint32_t and
    // SIGSEGV in bufferDrawTextBufferView. draw() must return before any
    // native call when the pane has no usable columns / scroll region.
    load_tui_design("");
    TUI tui;
    tui.set_rect(Rect{5, 1, 0, 0});
    tui.begin_input();
    CHECK(tui.cols() == 0);
    CHECK(tui.scroll_region_rows() <= 0);

    PaneScrollView view;
    view.append_prose(styled_user_echo_lines("should not paint"));
    // frame=1 is an invalid OpenTUI handle; a regression that skipped the
    // guard would crash or corrupt rather than return cleanly.
    view.draw(/*frame=*/1, tui, 0, 0);

    TUI offscreen;
    offscreen.set_rect(Rect{-10000, -10000, 0, 0});
    CHECK(offscreen.cols() == 0);
    view.draw(/*frame=*/1, offscreen, 0, 0);
}
