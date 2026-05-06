// tests/test_a2a.cpp — Unit tests for the A2A layer.
//
// Five slices, all in-process, no network IO:
//   1. Wire types round-trip — every struct serialises and re-parses.
//   2. JSON-RPC envelope — request, success response, error response.
//   3. SSE parser — chunked feeds dispatch the right events.
//   4. Agent-card builder — Constitution → AgentCard mapping is stable.
//   5. Event translator — emit_status / emit_text_chunk / emit_tool_call
//      produce the SSE frames an A2A client expects.
//
// Conformance against the upstream a2a-tck Python suite is a separate,
// network-bound integration target (run manually against `arbiter --api`).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "a2a/event_translator.h"
#include "a2a/server.h"
#include "a2a/sse_reader.h"
#include "a2a/types.h"
#include "constitution.h"
#include "json.h"

#include <atomic>
#include <string>
#include <vector>

// Pull only the A2A namespace into scope.  index_ai::Message (chat
// history struct) and index_ai::a2a::Message (protocol struct) collide
// when both are unqualified; we want the protocol one in this file.
using namespace index_ai::a2a;
using index_ai::Constitution;
using index_ai::JsonValue;
using index_ai::JsonArray;
using index_ai::JsonObject;
using index_ai::jbool;
using index_ai::jnum;
using index_ai::jstr;
using index_ai::jobj;
using index_ai::jarr;
using index_ai::jnull;
using index_ai::json_parse;
using index_ai::json_serialize;

// ── 1. Wire types round-trip ────────────────────────────────────────

TEST_CASE("Part text round-trips") {
    Part p;
    p.kind = "text";
    p.text = "hello world";
    auto j = to_json(p);
    auto back = part_from_json(*j);
    CHECK(back.kind == "text");
    CHECK(back.text == "hello world");
}

TEST_CASE("Part file with bytes round-trips") {
    Part p;
    p.kind        = "file";
    p.file_bytes  = "ZmlsZWNvbnRlbnRz";
    p.file_name   = "x.txt";
    p.file_mime_type = "text/plain";
    auto back = part_from_json(*to_json(p));
    CHECK(back.kind == "file");
    CHECK(back.file_bytes.value_or("") == "ZmlsZWNvbnRlbnRz");
    CHECK(back.file_name.value_or("")  == "x.txt");
    CHECK(back.file_mime_type.value_or("") == "text/plain");
    CHECK_FALSE(back.file_uri.has_value());
}

TEST_CASE("Part file with uri round-trips") {
    Part p;
    p.kind     = "file";
    p.file_uri = "https://example.com/x.pdf";
    auto back = part_from_json(*to_json(p));
    CHECK(back.file_uri.value_or("") == "https://example.com/x.pdf");
    CHECK_FALSE(back.file_bytes.has_value());
}

TEST_CASE("Part file rejects neither uri nor bytes") {
    auto bad = jobj();
    bad->as_object_mut()["kind"] = jstr("file");
    bad->as_object_mut()["file"] = jobj();
    CHECK_THROWS(part_from_json(*bad));
}

TEST_CASE("Part data round-trips with arbitrary JSON") {
    Part p;
    p.kind = "data";
    auto d = jobj();
    auto& m = d->as_object_mut();
    m["x"] = jnum(42);
    m["y"] = jstr("z");
    p.data = d;

    auto back = part_from_json(*to_json(p));
    CHECK(back.kind == "data");
    REQUIRE(back.data);
    CHECK(static_cast<int>(back.data->get_number("x", -1)) == 42);
    CHECK(back.data->get_string("y", "") == "z");
}

TEST_CASE("Message round-trips with all optional fields") {
    Message m;
    m.role        = "user";
    m.message_id  = "m-1";
    m.task_id     = "t-1";
    m.context_id  = "c-1";
    Part p1; p1.kind = "text"; p1.text = "hi";
    Part p2; p2.kind = "text"; p2.text = "again";
    m.parts.push_back(std::move(p1));
    m.parts.push_back(std::move(p2));
    auto md = jobj(); md->as_object_mut()["k"] = jstr("v");
    m.metadata = md;

    auto back = message_from_json(*to_json(m));
    CHECK(back.role == "user");
    CHECK(back.message_id == "m-1");
    CHECK(back.task_id.value_or("") == "t-1");
    CHECK(back.context_id.value_or("") == "c-1");
    REQUIRE(back.parts.size() == 2);
    CHECK(back.parts[0].text == "hi");
    CHECK(back.parts[1].text == "again");
    REQUIRE(back.metadata);
    CHECK(back.metadata->get_string("k", "") == "v");
}

TEST_CASE("Task round-trips with status + history") {
    Task t;
    t.id         = "t-42";
    t.context_id = "c-42";
    t.status.state = TaskState::completed;
    Message reply;
    reply.role        = "agent";
    reply.message_id  = "r-1";
    Part p; p.kind = "text"; p.text = "done";
    reply.parts.push_back(std::move(p));
    t.status.message = reply;
    t.history.push_back(reply);

    auto back = task_from_json(*to_json(t));
    CHECK(back.id == "t-42");
    CHECK(back.context_id == "c-42");
    CHECK(back.status.state == TaskState::completed);
    REQUIRE(back.status.message);
    REQUIRE_FALSE(back.status.message->parts.empty());
    CHECK(back.status.message->parts[0].text == "done");
    REQUIRE(back.history.size() == 1);
}

TEST_CASE("TaskState round-trip covers every enum value") {
    for (auto s : { TaskState::submitted, TaskState::working,
                    TaskState::input_required, TaskState::auth_required,
                    TaskState::completed, TaskState::canceled,
                    TaskState::failed, TaskState::rejected,
                    TaskState::unknown }) {
        CHECK(task_state_from_string(task_state_to_string(s)) == s);
    }
    CHECK(task_state_is_terminal(TaskState::completed));
    CHECK(task_state_is_terminal(TaskState::failed));
    CHECK_FALSE(task_state_is_terminal(TaskState::working));
}

// ── 2. JSON-RPC envelope ──────────────────────────────────────────────

TEST_CASE("RpcRequest serialises with jsonrpc/method/id/params") {
    RpcRequest r;
    r.id     = jnum(1);
    r.method = "message/send";
    auto params = jobj();
    params->as_object_mut()["k"] = jstr("v");
    r.params = params;

    auto j = to_json(r);
    CHECK(j->get_string("jsonrpc", "") == "2.0");
    CHECK(j->get_string("method", "")  == "message/send");
    CHECK(static_cast<int>(j->get_number("id", -1)) == 1);
    auto p = j->get("params");
    REQUIRE(p);
    CHECK(p->get_string("k", "") == "v");
}

TEST_CASE("RpcResponse success carries result and id") {
    auto res = jobj();
    res->as_object_mut()["ok"] = jbool(true);
    auto resp = make_result_response(jnum(7), res);
    auto j = to_json(resp);
    CHECK(j->get_string("jsonrpc", "") == "2.0");
    CHECK(static_cast<int>(j->get_number("id", -1)) == 7);
    auto r = j->get("result");
    REQUIRE(r);
    CHECK(r->get_bool("ok", false));
    CHECK_FALSE(j->get("error"));
}

TEST_CASE("RpcResponse error carries code + message and omits result") {
    auto resp = make_error_response(jnum(7), ERR_TASK_NOT_FOUND,
                                     "no such task");
    auto j = to_json(resp);
    auto err = j->get("error");
    REQUIRE(err);
    CHECK(static_cast<int>(err->get_number("code", 0)) == ERR_TASK_NOT_FOUND);
    CHECK(err->get_string("message", "") == "no such task");
    CHECK_FALSE(j->get("result"));
}

TEST_CASE("RpcResponse parses round-tripped success and error") {
    auto ok = jobj();
    ok->as_object_mut()["jsonrpc"] = jstr("2.0");
    ok->as_object_mut()["id"]      = jnum(3);
    auto res = jobj();
    res->as_object_mut()["x"] = jnum(1);
    ok->as_object_mut()["result"] = res;
    auto parsed = rpc_response_from_json(*ok);
    REQUIRE(parsed.result);
    CHECK_FALSE(parsed.error);

    auto bad = jobj();
    bad->as_object_mut()["jsonrpc"] = jstr("2.0");
    bad->as_object_mut()["id"]      = jnum(4);
    auto e = jobj();
    e->as_object_mut()["code"]    = jnum(-32601);
    e->as_object_mut()["message"] = jstr("nope");
    bad->as_object_mut()["error"] = e;
    auto parsed_err = rpc_response_from_json(*bad);
    CHECK_FALSE(parsed_err.result);
    REQUIRE(parsed_err.error);
    CHECK(parsed_err.error->code == -32601);
}

// ── 3. SSE parser ─────────────────────────────────────────────────────

TEST_CASE("SseReader dispatches one event per blank line") {
    std::vector<std::pair<std::string, std::string>> got;
    SseReader r([&](const std::string& ev, const std::string& data) {
        got.push_back({ev, data});
    });
    const char* stream =
        "event: message\n"
        "data: {\"a\":1}\n"
        "\n"
        "event: message\n"
        "data: {\"a\":2}\n"
        "\n";
    r.feed(stream, std::strlen(stream));
    REQUIRE(got.size() == 2);
    CHECK(got[0].first == "message");
    CHECK(got[0].second == "{\"a\":1}");
    CHECK(got[1].second == "{\"a\":2}");
}

TEST_CASE("SseReader joins multiple data lines with newlines") {
    std::vector<std::string> got;
    SseReader r([&](const std::string& /*ev*/, const std::string& data) {
        got.push_back(data);
    });
    const char* stream = "data: line1\ndata: line2\n\n";
    r.feed(stream, std::strlen(stream));
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "line1\nline2");
}

TEST_CASE("SseReader defaults event name to 'message'") {
    std::vector<std::string> got;
    SseReader r([&](const std::string& ev, const std::string& /*data*/) {
        got.push_back(ev);
    });
    const char* stream = "data: hi\n\n";
    r.feed(stream, std::strlen(stream));
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "message");
}

TEST_CASE("SseReader ignores comment lines") {
    std::vector<std::string> got;
    SseReader r([&](const std::string&, const std::string& d) {
        got.push_back(d);
    });
    const char* stream = ": keepalive\ndata: x\n\n: another\ndata: y\n\n";
    r.feed(stream, std::strlen(stream));
    REQUIRE(got.size() == 2);
    CHECK(got[0] == "x");
    CHECK(got[1] == "y");
}

TEST_CASE("SseReader handles arbitrary chunking") {
    std::vector<std::string> got;
    SseReader r([&](const std::string&, const std::string& d) {
        got.push_back(d);
    });
    const std::string stream =
        "event: message\ndata: {\"hello\":\"world\"}\n\n";
    // Feed one byte at a time.
    for (char c : stream) r.feed(&c, 1);
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "{\"hello\":\"world\"}");
}

// ── 4. Agent-card builder ─────────────────────────────────────────────

TEST_CASE("build_agent_card maps Constitution fields onto AgentCard") {
    Constitution c;
    c.name = "researcher";
    c.role = "research analyst";
    c.goal = "extract decisions and tradeoffs from technical RFCs";
    c.capabilities = { "/fetch", "/search", "/mem" };

    auto card = build_agent_card(c, "researcher",
                                  "https://example.com", "v123");
    CHECK(card.protocol_version == "1.0");
    CHECK(card.name == "researcher");
    CHECK(card.description.find("research analyst") != std::string::npos);
    CHECK(card.description.find("extract decisions") != std::string::npos);
    CHECK(card.url == "https://example.com/v1/a2a/agents/researcher");
    CHECK(card.version == "v123");
    CHECK(card.capabilities.streaming);
    CHECK_FALSE(card.capabilities.push_notifications);
    // chat is always first; per-capability skills follow in order.
    REQUIRE(card.skills.size() >= 4);
    CHECK(card.skills[0].id == "chat");
    CHECK(card.skills[1].id == "fetch-url");
    CHECK(card.skills[2].id == "web-search");
    CHECK(card.skills[3].id == "memory");
    REQUIRE(card.security_schemes);
    CHECK(card.preferred_transport.value_or("") == "JSONRPC");
}

TEST_CASE("build_well_known_stub has at least one skill (spec requires)") {
    auto card = build_well_known_stub("https://example.com");
    CHECK(card.protocol_version == "1.0");
    REQUIRE_FALSE(card.skills.empty());
    CHECK(card.skills[0].id == "discover");
    REQUIRE(card.security_schemes);
}

TEST_CASE("AgentCard parses back into the same shape") {
    Constitution c;
    c.name = "x";
    c.role = "y";
    auto card = build_agent_card(c, "x", "https://e.com", "1");
    auto j = to_json(card);
    auto back = agent_card_from_json(*j);
    CHECK(back.name == "x");
    CHECK(back.url == "https://e.com/v1/a2a/agents/x");
    CHECK(back.protocol_version == "1.0");
}

// ── 5. Event translator ──────────────────────────────────────────────

TEST_CASE("A2aStreamWriter emits status + artifact frames in JSON-RPC envelope") {
    std::vector<std::shared_ptr<JsonValue>> frames;
    EventSink sink = [&](const std::string& /*ev*/,
                          std::shared_ptr<JsonValue> p) {
        frames.push_back(p);
    };
    A2aStreamWriter w(sink, jnum(7), "task-1", "ctx-1", "researcher");

    w.emit_status(TaskState::working, /*final=*/false);
    w.emit_text_chunk("hello");
    w.emit_text_chunk(" world", /*last_chunk=*/true);
    w.emit_tool_call("fetch", /*ok=*/true);
    Message m;
    m.role = "agent"; m.message_id = "r-1";
    Part p; p.kind = "text"; p.text = "done"; m.parts.push_back(std::move(p));
    w.emit_status(TaskState::completed, /*final=*/true, m);

    REQUIRE(frames.size() == 5);
    // Every frame is a JSON-RPC response with jsonrpc/id/result.
    for (auto& f : frames) {
        REQUIRE(f);
        CHECK(f->get_string("jsonrpc", "") == "2.0");
        REQUIRE(f->get("result"));
        REQUIRE(f->get("id"));
    }
    // First frame: working status update.
    {
        auto r = frames[0]->get("result");
        CHECK(r->get_string("kind", "") == "status-update");
        CHECK(r->get_string("taskId", "") == "task-1");
        CHECK(r->get_string("contextId", "") == "ctx-1");
        auto status = r->get("status");
        REQUIRE(status);
        CHECK(status->get_string("state", "") == "working");
        CHECK(r->get_bool("final", true) == false);
    }
    // Second frame: first text chunk, append=false (establishes artifact).
    {
        auto r = frames[1]->get("result");
        CHECK(r->get_string("kind", "") == "artifact-update");
        CHECK(r->get_bool("append", true) == false);
        CHECK(r->get_bool("lastChunk", true) == false);
    }
    // Third frame: continuation chunk, append=true + lastChunk=true.
    {
        auto r = frames[2]->get("result");
        CHECK(r->get_bool("append", false) == true);
        CHECK(r->get_bool("lastChunk", false) == true);
    }
    // Fourth frame: tool call (data part).
    {
        auto r = frames[3]->get("result");
        CHECK(r->get_string("kind", "") == "artifact-update");
        auto art = r->get("artifact");
        REQUIRE(art);
        CHECK(art->get_string("name", "") == "tool_call");
    }
    // Fifth frame: terminal status with the assistant message attached.
    {
        auto r = frames[4]->get("result");
        CHECK(r->get_string("kind", "") == "status-update");
        CHECK(r->get_bool("final", false) == true);
        auto status = r->get("status");
        REQUIRE(status);
        CHECK(status->get_string("state", "") == "completed");
        REQUIRE(status->get("message"));
    }
}
