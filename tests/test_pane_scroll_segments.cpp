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

    // Five short lines → collapsed shows pad + header + kPreviewRows (3) +
    // ellipsis + pad.
    view.append_thinking("one\ntwo\nthree\nfour\nfive");
    const int collapsed = view.total_visual_rows() - baseline;
    // May include a leading block_gap blank; thinking chrome itself is 7 rows.
    CHECK(collapsed >= 7);
    CHECK(collapsed <= 9);

    CHECK(view.toggle_code_block_in_view(/*scroll_offset=*/0));
    const int expanded = view.total_visual_rows() - baseline;
    CHECK(expanded > collapsed);
    // Expanding replaces the ellipsis with the remaining body lines (+1 net).
    CHECK(expanded - collapsed == 1);
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
    CHECK(after_echo >= 3);  // pad + text + pad (+ textbuffer trailing nl)

    view.append_thinking("plan", /*new_block=*/true);
    const int after_think = view.total_visual_rows();
    // Exactly one BlankSegment between echo and thinking.
    // Thinking = pad + header + body + pad = 4 rows.
    CHECK(after_think - after_echo == 1 + 4);

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

    // Two tools share a cluster — only one gap before the first tool.
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

    // Without trim, soft blanks + block_gap would add 2+ visual empties.
    // With trim, the next block is only +gap + thinking rows beyond content.
    view.append_thinking("next", /*new_block=*/true);
    const int after = view.total_visual_rows();
    // Thinking chrome = pad + header + body + pad = 4; plus gap(1).
    CHECK(after - after_prose <= 5);
    CHECK(after - after_prose >= 4);  // at least gap + thinking chrome
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
