#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "tui/fleet.h"
#include "styled_text.h"
#include "tui/glyphs.h"

#include <algorithm>
#include <clocale>
#include <cstdlib>
#include <string>

using namespace arbiter;

namespace {

struct GlyphModeGuard {
    std::string prev;
    bool had = false;
    explicit GlyphModeGuard(const char* mode) {
        if (const char* p = std::getenv("ARBITER_GLYPHS")) {
            had = true;
            prev = p;
        }
        setenv("ARBITER_GLYPHS", mode, 1);
    }
    ~GlyphModeGuard() {
        if (had) setenv("ARBITER_GLYPHS", prev.c_str(), 1);
        else unsetenv("ARBITER_GLYPHS");
    }
};

struct EnvVarGuard {
    std::string key;
    std::string prev;
    bool had = false;
    EnvVarGuard(const char* k, const char* value) : key(k) {
        if (const char* p = std::getenv(k)) {
            had = true;
            prev = p;
        }
        if (value) setenv(k, value, 1);
        else unsetenv(k);
    }
    ~EnvVarGuard() {
        if (had) setenv(key.c_str(), prev.c_str(), 1);
        else unsetenv(key.c_str());
    }
};

// The suite asserts unicode shapes unless a case opts into ascii/auto.
struct ForceUnicodeGlyphs {
    ForceUnicodeGlyphs() { setenv("ARBITER_GLYPHS", "unicode", 1); }
};
[[maybe_unused]] const ForceUnicodeGlyphs kForceUnicodeGlyphs;

}  // namespace

TEST_CASE("stream_start opens a row; stream_end marks it ended but keeps it") {
    FleetTree t;
    t.on_stream_start("index", 0, 0);
    auto n = t.find_stream(0);
    REQUIRE(n);
    CHECK(n->agent == "index");
    CHECK(n->depth == 0);
    CHECK(n->running);
    CHECK_FALSE(n->ended);

    t.on_stream_end(0, true);
    n = t.find_stream(0);
    REQUIRE(n);
    CHECK(n->ended);
    CHECK(n->ok);
    CHECK_FALSE(n->running);
    CHECK_FALSE(t.empty());
}

TEST_CASE("parallel children indent by depth and close independently") {
    FleetTree t;
    t.on_stream_start("index", 0, 0);
    t.on_stream_start("researcher_a", 1, 1);
    t.on_stream_start("researcher_b", 2, 1);
    t.on_stream_start("researcher_c", 3, 1);

    auto rows = t.paint_rows(28);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].agent == "index");
    CHECK(rows[0].depth == 0);
    CHECK(rows[1].depth == 1);
    CHECK(rows[2].depth == 1);
    CHECK(rows[3].depth == 1);

    t.on_stream_end(2, true);
    auto b = t.find_stream(2);
    REQUIRE(b);
    CHECK(b->ended);
    CHECK(t.find_stream(1)->running);
    CHECK(t.has_live());
}

TEST_CASE("depth-2 child attaches under a depth-1 parent") {
    FleetTree t;
    t.on_stream_start("index", 0, 0);
    t.on_stream_start("lead", 1, 1);
    t.on_stream_start("helper", 2, 2);
    auto h = t.find_stream(2);
    REQUIRE(h);
    CHECK(h->parent_stream_id == 1);
    auto rows = t.paint_rows(28);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].agent == "index");
    CHECK(rows[1].agent == "lead");
    CHECK(rows[2].agent == "helper");
    CHECK(rows[2].depth == 2);
}

TEST_CASE("depth cap paints 2 even if an event claims 3") {
    FleetTree t;
    t.on_stream_start("index", 0, 0);
    t.on_stream_start("too_deep", 1, 3);
    auto n = t.find_stream(1);
    REQUIRE(n);
    CHECK(n->depth == 2);
}

TEST_CASE("tool_call sets and clears the active tool") {
    FleetTree t;
    t.on_stream_start("index", 0, 0);
    t.on_tool_call(0, "fetch", true);
    CHECK(t.find_stream(0)->active_tool == "fetch");
    t.on_tool_call(0, "fetch", false);
    CHECK(t.find_stream(0)->active_tool.empty());
}

TEST_CASE("token_usage aggregates per stream and across the tree") {
    FleetTree t;
    t.on_stream_start("index", 0, 0);
    t.on_stream_start("researcher_a", 1, 1);
    t.on_token_usage(0, 100, 20);
    t.on_token_usage(0, 50, 10);
    t.on_token_usage(1, 800, 120);
    CHECK(t.find_stream(0)->input_tokens == 150);
    CHECK(t.find_stream(0)->output_tokens == 30);
    CHECK(t.find_stream(1)->input_tokens == 800);
    CHECK(t.total_input() == 950);
    CHECK(t.total_output() == 150);
}

TEST_CASE("agent.spawned opens a JIT row; agent.teardown removes it") {
    FleetTree t;
    t.on_agent_spawned("writer", "clone-1", {"readme"});
    auto n = t.find_clone("clone-1");
    REQUIRE(n);
    CHECK(n->ephemeral);
    CHECK(n->running);
    CHECK(n->agent == "writer");
    CHECK(n->clauses.size() == 1);

    auto rows = t.paint_rows(28);
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].ephemeral);
    CHECK(rows[0].line.find("\u25C7") != std::string::npos);  // ◇

    t.on_agent_teardown("clone-1");
    CHECK_FALSE(t.find_clone("clone-1"));
    CHECK(t.nodes().empty());
}

TEST_CASE("stream_start after spawn attaches stream_id to the clone") {
    FleetTree t;
    t.on_agent_spawned("writer", "clone-9", {"license"});
    t.on_stream_start("writer", 4, 1);
    auto n = t.find_clone("clone-9");
    REQUIRE(n);
    CHECK(n->stream_id == 4);
    CHECK(t.find_stream(4)->ephemeral);
}

TEST_CASE("reconcile.delta overlay tracks residual vs held and wave") {
    FleetTree t;
    t.on_reconcile_delta(3, 2, false, 1, "req-1");
    auto ov = t.overlay();
    CHECK(ov.active);
    CHECK(ov.residual == 3);
    CHECK(ov.held == 2);
    CHECK(ov.wave == 1);
    CHECK_FALSE(ov.empty);
    auto lines = format_fleet_overlay(ov, 24);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].find("wave 1") != std::string::npos);
    CHECK(lines[1].find("3 residual") != std::string::npos);
    CHECK(lines[1].find("2 held") != std::string::npos);
}

TEST_CASE("SSE fixtures drive the same transitions") {
    FleetTree t;
    t.ingest_sse("request_received",
                 R"({"request_id":"r1","agent":"index"})");
    t.ingest_sse("stream_start",
                 R"({"agent":"index","stream_id":0,"depth":0})");
    t.ingest_sse("stream_start",
                 R"({"agent":"a","stream_id":1,"depth":1})");
    t.ingest_sse("stream_start",
                 R"({"agent":"b","stream_id":2,"depth":1})");
    t.ingest_sse("agent_start",
                 R"({"agent":"a","stream_id":1,"depth":1})");
    t.ingest_sse("token_usage",
                 R"({"stream_id":1,"input_tokens":10,"output_tokens":4})");
    t.ingest_sse("stream_end",
                 R"({"stream_id":1,"ok":true})");
    t.ingest_sse("stream_end",
                 R"({"stream_id":2,"ok":false})");

    CHECK(t.find_stream(1)->ended);
    CHECK(t.find_stream(1)->ok);
    CHECK_FALSE(t.find_stream(2)->ok);
    CHECK(t.total_input() == 10);
    CHECK(t.total_output() == 4);
}

TEST_CASE("SSE agent.spawned / teardown and reconcile.delta") {
    FleetTree t;
    t.ingest_sse("request_received",
                 R"({"request_id":"rec-1","agent":"reconcile"})");
    t.ingest_sse("reconcile.delta",
                 R"({"residual":[{"id":"c1"}],"held":[{"id":"c2"},{"id":"c3"}],"empty":false,"wave":0})");
    t.ingest_sse("agent.spawned",
                 R"({"agent":"nexus","clone_id":"cl-a","clauses":["c1"]})");
    CHECK(t.overlay().residual == 1);
    CHECK(t.overlay().held == 2);
    REQUIRE(t.find_clone("cl-a"));
    t.ingest_sse("agent.teardown", R"({"agent":"nexus","clone_id":"cl-a"})");
    CHECK_FALSE(t.find_clone("cl-a"));
}

TEST_CASE("new request_id clears the previous job") {
    FleetTree t;
    t.on_stream_start("index", 0, 0, "r1");
    t.on_stream_start("child", 1, 1, "r1");
    CHECK(t.nodes().size() == 2);
    t.on_request_received("r2", "index");
    CHECK(t.nodes().empty());
}

TEST_CASE("format_fleet_row is scannable and width-capped") {
    FleetNode n;
    n.agent = "researcher_a";
    n.depth = 1;
    n.running = true;
    n.active_tool = "fetch";
    n.input_tokens = 1200;
    const std::string line = format_fleet_row(n, true, true, 24);
    CHECK(line.size() > 0);
    CHECK(static_cast<int>(display_width(line)) <= 24);
    CHECK(line.find("researcher_a") != std::string::npos);
}

TEST_CASE("fleet sidebar stays hidden until there is a job") {
    FleetTree t;
    FleetSidebarState sb;
    CHECK(sb.effective_width(120, 0, false) == 0);
    t.on_stream_start("index", 0, 0);
    CHECK(sb.effective_width(120, 0, true) == 28);
    CHECK(sb.effective_width(80, 0, true) == 0);
    sb.toggle_visible();
    CHECK(sb.effective_width(120, 0, true) == 0);
}

TEST_CASE("done marks leftover running rows ended") {
    FleetTree t;
    t.on_stream_start("index", 0, 0);
    t.on_stream_start("a", 1, 1);
    t.on_done();
    CHECK(t.find_stream(0)->ended);
    CHECK(t.find_stream(1)->ended);
    CHECK_FALSE(t.has_live());
}

TEST_CASE("five-agent parallel job paints a depth-indented tree") {
    FleetTree t;
    t.on_stream_start("index", 0, 0);
    t.on_stream_start("researcher_a", 1, 1);
    t.on_stream_start("researcher_b", 2, 1);
    t.on_stream_start("writer", 3, 1);
    t.on_stream_start("reviewer", 4, 1);
    t.on_tool_call(1, "fetch", true);
    t.on_token_usage(2, 400, 80);
    t.on_token_usage(3, 200, 40);
    t.on_stream_end(4, true);

    auto rows = t.paint_rows(28);
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].agent == "index");
    CHECK(rows[0].depth == 0);
    CHECK(rows[1].depth == 1);
    CHECK(rows[2].depth == 1);
    CHECK(rows[3].depth == 1);
    CHECK(rows[4].depth == 1);
    CHECK(rows[1].running);
    CHECK(rows[1].line.find("fetch") != std::string::npos);
    CHECK(rows[4].ended);
    CHECK(t.has_live());
    CHECK(t.total_input() == 600);
    CHECK(t.total_output() == 120);
}

TEST_CASE("ensure wave overlay sits with JIT spawn and teardown rows") {
    FleetTree t;
    t.on_request_received("rec-9", "reconcile");
    t.on_reconcile_delta(3, 2, false, 2, "rec-9");
    t.on_agent_spawned("writer", "cl-1", {"readme"});
    t.on_agent_spawned("reviewer", "cl-2", {"license"});
    t.on_stream_start("writer", 7, 1);

    auto ov = t.overlay();
    CHECK(ov.active);
    CHECK(ov.mode == "ensure");
    CHECK(ov.wave == 2);
    CHECK(ov.residual == 3);
    CHECK(ov.held == 2);

    auto rows = t.paint_rows(28);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].ephemeral);
    CHECK(rows[0].stream_id == 7);
    CHECK(rows[1].ephemeral);
    CHECK(rows[1].clone_id == "cl-2");

    t.on_agent_teardown("cl-1");
    CHECK_FALSE(t.find_clone("cl-1"));
    CHECK(t.find_clone("cl-2"));
    CHECK(t.paint_rows(28).size() == 1);
}

TEST_CASE("sidebar snapshot clamps selection to live row count") {
    FleetTree t;
    FleetSidebarState sb;
    t.on_stream_start("index", 0, 0);
    t.on_stream_start("a", 1, 1);
    t.on_stream_start("b", 2, 1);
    sb.select_at_index(2, 10);
    auto snap = sb.snapshot(t, 120, 0);
    CHECK(snap.visible);
    CHECK(snap.selected == 2);
    CHECK(snap.rows.size() == 3);

    t.on_agent_teardown("");  // no-op
    t.clear();
    snap = sb.snapshot(t, 120, 0);
    CHECK(snap.selected == 0);
    CHECK(snap.rows.empty());
    CHECK_FALSE(snap.visible);
}

TEST_CASE("fleet pane height tracks content, not a fixed fraction of the terminal") {
    const int one = fleet_sidebar_chrome_rows(0) + 1;
    CHECK(one == 5);
    CHECK(fleet_sidebar_list_rows(one, 0) == 1);

    // 0 and 1 agents are the same short box on every normal height.
    // The previous rail used max(10, rows * 2 / 5), about the top half.
    for (int rows : {24, 40, 50, 80}) {
        CHECK(fleet_sidebar_height(rows, 0, 0, true) == one);
        CHECK(fleet_sidebar_height(rows, 1, 0, true) == one);
        CHECK(fleet_sidebar_height(rows, 1, 0, true) < std::max(10, rows * 2 / 5));
        CHECK(rows - one >= kFleetSessionMinRows);
    }

    // Many agents: cap the list and scroll; Session keeps its floor.
    const int many_40 = fleet_sidebar_height(40, 30, 0, true);
    CHECK(many_40 == fleet_sidebar_chrome_rows(0) + kFleetMaxListRows);
    CHECK(fleet_sidebar_list_rows(many_40, 0) == kFleetMaxListRows);
    CHECK(fleet_sidebar_list_rows(many_40, 0) < 30);
    CHECK(40 - many_40 >= kFleetSessionMinRows);

    const int many_80 = fleet_sidebar_height(80, 100, 0, true);
    CHECK(many_80 == many_40);
    CHECK(80 - many_80 >= kFleetSessionMinRows);

    // 24 rows cannot fit 12 list rows and a 14-row session. Fleet shrinks.
    const int many_24 = fleet_sidebar_height(24, 30, 0, true);
    CHECK(many_24 < many_40);
    CHECK(fleet_sidebar_list_rows(many_24, 0) >= 1);
    CHECK(fleet_sidebar_list_rows(many_24, 0) < 30);
    CHECK(24 - many_24 == kFleetSessionMinRows);

    // Tight terminals still show one fleet row and give the rest away.
    CHECK(fleet_sidebar_height(10, 1, 0, true) == one);
    CHECK(fleet_sidebar_height(10, 20, 0, true) == one);
    CHECK(10 - one >= 1);
    CHECK(fleet_sidebar_height(6, 20, 0, true) == one);

    // Reconcile overlay is inside the content height, not extra empty space.
    const int with_overlay = fleet_sidebar_height(40, 1, 2, true);
    CHECK(with_overlay == fleet_sidebar_chrome_rows(2) + 1);
    CHECK(with_overlay > one);
    CHECK(fleet_sidebar_list_rows(with_overlay, 2) == 1);
    const int many_overlay = fleet_sidebar_height(40, 30, 2, true);
    CHECK(fleet_sidebar_list_rows(many_overlay, 2) == kFleetMaxListRows);
    CHECK(40 - many_overlay >= kFleetSessionMinRows);

    // Multi-pane hides Session. Fleet still sizes to content instead of
    // taking the whole column.
    CHECK(fleet_sidebar_height(40, 0, 0, false) == one);
    CHECK(fleet_sidebar_height(40, 1, 0, false) == one);
    CHECK(fleet_sidebar_height(80, 1, 0, false) == one);
    const int solo_many = fleet_sidebar_height(40, 30, 0, false);
    CHECK(solo_many == fleet_sidebar_chrome_rows(0) + kFleetMaxListRows);
    CHECK(solo_many < 40);

    CHECK(fleet_sidebar_height(0, 1, 0, true) == 0);
    CHECK(fleet_sidebar_height(-4, 3, 1, false) == 0);
    CHECK(fleet_sidebar_list_rows(0, 0) == 0);
    CHECK(fleet_sidebar_list_rows(one, 2) == 0);
}

TEST_CASE("cancelled fleet nodes are not painted as failures") {
    GlyphModeGuard mode("unicode");
    FleetTree t;
    t.on_stream_start("loom", 1, 0);
    t.on_stream_end(1, FleetTree::Terminal::Cancelled);
    auto n = t.find_stream(1);
    REQUIRE(n);
    CHECK(n->cancelled);
    CHECK(n->ended);
    CHECK_FALSE(n->ok);
    CHECK(fleet_node_status(*n) == RunState::Cancelled);
    auto rows = t.paint_rows(28);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].status == RunState::Cancelled);
    CHECK(rows[0].line.find(glyphs().cancelled) != std::string::npos);
    CHECK(rows[0].line.find(glyphs().failed) == std::string::npos);

    t.ingest_sse("stream_end",
                 R"({"stream_id":1,"ok":false,"status":"cancelled"})");
    CHECK(t.find_stream(1)->cancelled);
}

TEST_CASE("waiting fleet nodes use the dotted glyph and a blocked-on tail") {
    GlyphModeGuard mode("unicode");
    FleetTree t;
    t.on_stream_start("vera", 2, 0);
    t.on_waiting(2, "nexus");
    auto n = t.find_stream(2);
    REQUIRE(n);
    CHECK(n->waiting);
    CHECK_FALSE(n->running);
    CHECK(fleet_node_status(*n) == RunState::Waiting);
    CHECK(t.has_live());
    auto rows = t.paint_rows(32);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].status == RunState::Waiting);
    CHECK(rows[0].line.find(glyphs().waiting) != std::string::npos);
    CHECK(rows[0].line.find("waiting on nexus") != std::string::npos);

    t.on_text(2);
    CHECK(fleet_node_status(*t.find_stream(2)) == RunState::Running);
    t.ingest_sse("agent_wait", R"({"stream_id":2,"on":"nexus"})");
    CHECK(fleet_node_status(*t.find_stream(2)) == RunState::Waiting);
}

TEST_CASE("ascii glyph mode keeps fleet marks in single-byte ASCII") {
    GlyphModeGuard mode("ascii");
    CHECK(glyph_mode() == GlyphMode::Ascii);
    FleetTree t;
    t.on_stream_start("index", 0, 0);
    t.on_agent_spawned("quill", "c1", {"clause"});
    t.on_stream_start("quill", 3, 1);
    t.on_waiting(3, "nexus");
    auto rows = t.paint_rows(40);
    REQUIRE(rows.size() >= 2);
    const std::string& jit = rows.back().line;
    CHECK(jit.find('^') != std::string::npos);
    CHECK(jit.find('~') != std::string::npos);
    for (unsigned char c : jit) CHECK(c < 128);

    FleetNode failed;
    failed.agent = "forge";
    failed.ended = true;
    failed.ok = false;
    const std::string bad = format_fleet_row(failed, true, true, 24);
    CHECK(bad.find('x') != std::string::npos);

    FleetNode cancelled;
    cancelled.agent = "loom";
    cancelled.ended = true;
    cancelled.ok = false;
    cancelled.cancelled = true;
    const std::string stop = format_fleet_row(cancelled, true, true, 24);
    CHECK(stop.find('/') != std::string::npos);
    CHECK(stop.find('x') == std::string::npos);
}

TEST_CASE("unicode state glyphs are one cell and ascii mode is plain bytes") {
    GlyphModeGuard unicode("unicode");
    const char* prev_locale = std::setlocale(LC_ALL, nullptr);
    const std::string saved_locale = prev_locale ? prev_locale : "C";
    if (!std::setlocale(LC_ALL, "C.UTF-8")) {
        std::setlocale(LC_ALL, saved_locale.c_str());
    }
    const char* marks[] = {
        glyphs().pending, glyphs().running, glyphs().waiting,
        glyphs().succeeded, glyphs().failed, glyphs().cancelled,
        glyphs().advisor_halt, glyphs().advisor_redirect, glyphs().advisor_consult,
        glyphs().intent_applied, glyphs().intent_hint, glyphs().presence,
        glyphs().jit, glyphs().activity, glyphs().scheduled, glyphs().paused,
        glyphs().loop, glyphs().folder_open, glyphs().folder_closed,
        glyphs().task_todo, glyphs().task_done, glyphs().separator,
        glyphs().handoff, glyphs().tree_vert, glyphs().tree_branch,
        glyphs().tree_last,
    };
    for (const char* m : marks) {
        CHECK(display_width(m) == 1);
    }
    GlyphModeGuard ascii("ascii");
    const char* ascii_marks[] = {
        glyphs().pending, glyphs().running, glyphs().waiting,
        glyphs().succeeded, glyphs().failed, glyphs().cancelled,
        glyphs().advisor_halt, glyphs().advisor_redirect, glyphs().advisor_consult,
        glyphs().intent_applied, glyphs().intent_hint, glyphs().presence,
        glyphs().jit, glyphs().activity, glyphs().scheduled, glyphs().paused,
        glyphs().loop, glyphs().folder_open, glyphs().folder_closed,
        glyphs().task_todo, glyphs().task_done, glyphs().separator,
        glyphs().handoff, glyphs().tree_vert, glyphs().tree_branch,
        glyphs().tree_last,
    };
    for (const char* m : ascii_marks) {
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(m); *p; ++p) {
            CHECK(*p < 128);
        }
    }
    std::setlocale(LC_ALL, saved_locale.c_str());
}

TEST_CASE("glyph auto mode follows TERM and the locale") {
    GlyphModeGuard glyphs_env("auto");
    {
        EnvVarGuard term("TERM", "dumb");
        CHECK(glyph_mode() == GlyphMode::Ascii);
    }
    {
        EnvVarGuard term("TERM", "xterm-256color");
        EnvVarGuard lc("LC_ALL", "C");
        CHECK(glyph_mode() == GlyphMode::Ascii);
    }
    {
        EnvVarGuard term("TERM", "xterm-256color");
        EnvVarGuard lc("LC_ALL", "en_US.UTF-8");
        CHECK(glyph_mode() == GlyphMode::Unicode);
    }
    GlyphModeGuard forced("unicode");
    EnvVarGuard term("TERM", "dumb");
    CHECK(glyph_mode() == GlyphMode::Unicode);
}
