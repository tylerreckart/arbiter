#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "tui/fleet.h"
#include "styled_text.h"

#include <string>

using namespace arbiter;

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
