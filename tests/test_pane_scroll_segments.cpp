#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "commands.h"
#include "tui/opentui/pane_scroll_view.h"
#include "tui/tui.h"

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
    TUI tui;
    PaneScrollView view;
    bind_view(view, tui, 40, 40);
    const int baseline = view.total_visual_rows();

    // Five short lines → collapsed shows header + kPreviewRows (3) + ellipsis.
    view.append_thinking("one\ntwo\nthree\nfour\nfive");
    const int collapsed = view.total_visual_rows() - baseline;
    // May include a leading block_gap blank; the thinking chrome itself is 5 rows.
    CHECK(collapsed >= 5);
    CHECK(collapsed <= 7);

    CHECK(view.toggle_code_block_in_view(/*scroll_offset=*/0));
    const int expanded = view.total_visual_rows() - baseline;
    CHECK(expanded > collapsed);
    // Expanding replaces the ellipsis with the remaining body lines (+1 net).
    CHECK(expanded - collapsed == 1);
}

TEST_CASE("ThinkingSegment wrap width grows visual rows for long lines") {
    TUI tui_n;
    PaneScrollView narrow;
    bind_view(narrow, tui_n, 20, 40);
    narrow.append_thinking(
        "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOP");

    TUI tui_w;
    PaneScrollView wide;
    bind_view(wide, tui_w, 80, 40);
    wide.append_thinking(
        "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOP");

    CHECK(narrow.total_visual_rows() >= wide.total_visual_rows());

    CHECK(narrow.toggle_code_block_in_view(0));
    CHECK(wide.toggle_code_block_in_view(0));
    CHECK(narrow.total_visual_rows() > wide.total_visual_rows());
}
