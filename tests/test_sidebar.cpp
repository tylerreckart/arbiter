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
    CHECK(sb.effective_width(120, 1, 27) == 0);
    CHECK(sb.effective_width(147, 1, 27) == 28);
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
    CHECK(wide.x == 120 - 28 - SidebarState::kOuterGutter);
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

TEST_CASE("todo add adopts DB id from result preview; done marks completed") {
    SidebarState sb;
    sb.record_tool("todo:add Ship the landing page", true,
                   "OK: added #42 — Ship the landing page");
    {
        const auto snap = sb.snapshot();
        REQUIRE(snap.todos.size() == 1);
        CHECK(snap.todos[0].id == 42);
        CHECK(snap.todos[0].status == "pending");
        CHECK(snap.todos[0].subject == "Ship the landing page");
    }
    sb.record_tool("todo:start 42", true, "OK: in_progress — Ship the landing page");
    CHECK(sb.snapshot().todos[0].status == "in_progress");
    sb.record_tool("todo:done 42", true, "OK: completed — Ship the landing page");
    {
        const auto snap = sb.snapshot();
        REQUIRE(snap.todos.size() == 1);
        CHECK(snap.todos[0].status == "completed");
    }
}

TEST_CASE("todo start/done accept subject when agent omits the id") {
    SidebarState sb;
    sb.record_tool("todo:add Analyze Nabonidus Chronicle", true,
                   "OK: added #7 — Analyze Nabonidus Chronicle");
    sb.record_tool("todo:start Analyze Nabonidus Chronicle", true,
                   "OK: in_progress — Analyze Nabonidus Chronicle");
    CHECK(sb.snapshot().todos[0].status == "in_progress");
    sb.record_tool("todo:done Analyze Nabonidus", true,
                   "OK: completed — Analyze Nabonidus Chronicle");
    CHECK(sb.snapshot().todos[0].status == "completed");
}

TEST_CASE("todo subject replay resolves target by persisted subject ref") {
    SidebarState sb;
    sb.record_tool("todo:add Task A", true, "OK: added #1 — Task A");
    sb.record_tool("todo:add Task B", true, "OK: added #2 — Task B");
    sb.record_tool("todo:start 1", true, "OK: in_progress — Task A");
    CHECK(sb.persist_todo_label("todo:subject 2: Renamed B")
          == "todo:subject Task B: Renamed B");
    sb.record_tool("todo:subject Task B: Renamed B", true);
    const auto snap = sb.snapshot();
    REQUIRE(snap.todos.size() == 2);
    const SidebarTodoEntry* a = nullptr;
    const SidebarTodoEntry* b = nullptr;
    for (const auto& t : snap.todos) {
        if (t.id == 1) a = &t;
        if (t.id == 2) b = &t;
    }
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->subject == "Task A");
    CHECK(a->status == "in_progress");
    CHECK(b->subject == "Renamed B");
    CHECK(b->status == "pending");
}

TEST_CASE("friendly_todo_label hides numeric ids from user-facing chrome") {
    SidebarState sb;
    sb.record_tool("todo:add Ship the landing page", true,
                   "OK: added #42 — Ship the landing page");
    CHECK(sb.friendly_todo_label("todo:start 42")
          == "todo:start Ship the landing page");
    CHECK(sb.friendly_todo_label("todo:done #42")
          == "todo:done Ship the landing page");
    CHECK(sb.friendly_todo_label("todo:add Ship the landing page")
          == "todo:add Ship the landing page");
}
