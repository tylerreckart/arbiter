#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "tui/sidebar.h"

using namespace arbiter;

TEST_CASE("breakpoint_width respects terminal width tiers") {
    CHECK(SidebarState::breakpoint_width(80) == 0);
    CHECK(SidebarState::breakpoint_width(95) == 0);
    CHECK(SidebarState::breakpoint_width(96) == 24);
    CHECK(SidebarState::breakpoint_width(119) == 24);
    CHECK(SidebarState::breakpoint_width(120) == 28);
}

TEST_CASE("effective_width gates on session, visibility, and pane count") {
    SidebarState sb;

    CHECK(sb.effective_width(120, 1) == 0);

    sb.mark_prompt_started();
    CHECK(sb.effective_width(120, 1) == 28);
    CHECK(sb.effective_width(100, 1) == 24);
    CHECK(sb.effective_width(80, 1) == 0);

    CHECK(sb.effective_width(120, 2) == 0);

    sb.toggle_visible();
    CHECK(sb.effective_width(120, 1) == 0);

    sb.toggle_visible();
    CHECK(sb.effective_width(120, 1) == 28);
}

TEST_CASE("effective_width subtracts leading history sidebar columns") {
    SidebarState sb;
    sb.mark_prompt_started();
    CHECK(sb.effective_width(120, 1, 26) == 0);
    CHECK(sb.effective_width(146, 1, 26) == 28);
}

TEST_CASE("rect_for_terminal is empty when sidebar is hidden") {
    SidebarState sb;
    sb.mark_prompt_started();

    const Rect narrow = sb.rect_for_terminal(80, 40, 1);
    CHECK(narrow.w == 0);
    CHECK(narrow.h == 0);

    const Rect multi = sb.rect_for_terminal(120, 40, 2);
    CHECK(multi.w == 0);

    const Rect wide = sb.rect_for_terminal(120, 40, 1);
    CHECK(wide.w == 28);
    CHECK(wide.x == 92);
    CHECK(wide.h == 40);
}

TEST_CASE("mcp recent list stays empty until an mcp tool is recorded") {
    SidebarState sb;
    CHECK(sb.snapshot().mcp.empty());
    sb.record_tool("bash", true);
    CHECK(sb.snapshot().mcp.empty());
    CHECK(sb.snapshot().tools.size() == 1);
    sb.record_tool("mcp:filesystem/list", true);
    const auto snap = sb.snapshot();
    CHECK(snap.mcp.size() == 1);
    CHECK(snap.mcp[0].name == "filesystem/list");
    CHECK(snap.mcp[0].ok);
}
