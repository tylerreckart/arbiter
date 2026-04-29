// tests/test_mcp.cpp — Unit tests for the MCP layer
//
// Three slices, all subprocess-light:
//   1. JSON-RPC framing — encode/decode round-trip without any IO.
//   2. Subprocess wrapper — spawn /bin/cat (a trivial echo) and exercise
//      send_line / recv_line / terminate.  No MCP semantics involved.
//   3. /mcp slash dispatch — feed parse_agent_commands a sample turn,
//      run it through execute_agent_commands with a stub MCPInvoker,
//      and check the rendered tool-result block.
//
// Live playwright is intentionally not exercised here — that's a manual
// smoke test (operator runs `arbiter --api`, then a /v1/orchestrate call
// emits `/mcp call playwright browser_navigate {"url":"..."}`).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "commands.h"
#include "json.h"
#include "mcp/subprocess.h"
#include "mcp/types.h"

#include <chrono>
#include <thread>

using namespace index_ai;
using namespace index_ai::mcp;
using namespace std::chrono_literals;

// ── 1. JSON-RPC framing ─────────────────────────────────────────────

TEST_CASE("Request serializes with jsonrpc/id/method/params") {
    Request r;
    r.id     = 7;
    r.method = "initialize";
    auto p = jobj();
    p->as_object_mut()["protocolVersion"] = jstr("2025-06-18");
    r.params = p;

    auto wire = serialize_request(r);
    auto v = json_parse(wire);
    REQUIRE(v);
    REQUIRE(v->is_object());
    CHECK(v->get_string("jsonrpc", "") == "2.0");
    CHECK(v->get_string("method",  "") == "initialize");
    CHECK(static_cast<int>(v->get_number("id", -1)) == 7);
    auto params = v->get("params");
    REQUIRE(params);
    CHECK(params->get_string("protocolVersion", "") == "2025-06-18");
}

TEST_CASE("Notification serializes without an id") {
    Notification n;
    n.method = "notifications/initialized";
    n.params = jobj();
    auto wire = serialize_notification(n);
    auto v = json_parse(wire);
    REQUIRE(v);
    CHECK(v->get_string("method", "") == "notifications/initialized");
    CHECK(v->get("id") == nullptr);   // notifications must NOT carry an id
}

TEST_CASE("parse_response: success case") {
    auto resp = parse_response(R"({"jsonrpc":"2.0","id":3,"result":{"x":1}})");
    CHECK(resp.id == 3);
    REQUIRE(resp.result);
    CHECK(static_cast<int>(resp.result->get_number("x", 0)) == 1);
    CHECK_FALSE(resp.error.has_value());
}

TEST_CASE("parse_response: error case") {
    auto resp = parse_response(R"({"jsonrpc":"2.0","id":4,"error":{"code":-32601,"message":"no such method"}})");
    CHECK(resp.id == 4);
    REQUIRE(resp.error.has_value());
    CHECK(resp.error->code == -32601);
    CHECK(resp.error->message == "no such method");
}

TEST_CASE("parse_response rejects malformed envelopes") {
    CHECK_THROWS(parse_response("not json"));
    CHECK_THROWS(parse_response(R"({"id":1,"result":{}})"));         // missing jsonrpc
    CHECK_THROWS(parse_response(R"({"jsonrpc":"1.0","id":1,"result":{}})")); // wrong version
    CHECK_THROWS(parse_response(R"({"jsonrpc":"2.0","id":1,"result":{},"error":{"code":1,"message":"x"}})")); // both
}

TEST_CASE("parse_tools_list extracts name + description + schema") {
    auto resp_v = json_parse(R"({"jsonrpc":"2.0","id":1,"result":{"tools":[
        {"name":"navigate","description":"Open a URL","inputSchema":{"type":"object"}},
        {"name":"click","inputSchema":{"type":"object","properties":{"ref":{"type":"string"}}}}
    ]}})");
    Response r;
    r.id = 1;
    r.result = resp_v->get("result");
    auto tools = parse_tools_list(r);
    REQUIRE(tools.size() == 2);
    CHECK(tools[0].name == "navigate");
    CHECK(tools[0].description == "Open a URL");
    REQUIRE(tools[0].input_schema);
    CHECK(tools[0].input_schema->is_object());
    CHECK(tools[1].name == "click");
    CHECK(tools[1].description.empty());     // missing field is "" not error
}

TEST_CASE("parse_tool_result folds JSON-RPC error into ToolResult") {
    Response r;
    r.error = RpcError{ -32602, "invalid params", nullptr };
    auto out = parse_tool_result(r);
    CHECK(out.is_error);
    REQUIRE(out.content.size() == 1);
    CHECK(out.content[0].type == "text");
    CHECK(out.content[0].text.find("invalid params") != std::string::npos);
}

TEST_CASE("parse_tool_result extracts content array + isError") {
    auto v = json_parse(R"({"isError":false,"content":[
        {"type":"text","text":"hello"},
        {"type":"image","mimeType":"image/png"}
    ]})");
    Response r;
    r.result = v;
    auto out = parse_tool_result(r);
    CHECK_FALSE(out.is_error);
    REQUIRE(out.content.size() == 2);
    CHECK(out.content[0].type == "text");
    CHECK(out.content[0].text == "hello");
    CHECK(out.content[1].type == "image");
    CHECK(out.content[1].mime_type == "image/png");
}

TEST_CASE("render_tool_result concatenates text and tags non-text") {
    ToolResult tr;
    tr.content.push_back({"text", "first line",    ""});
    tr.content.push_back({"text", "second line\n", ""});
    tr.content.push_back({"image", "",             "image/png"});

    auto body = render_tool_result(tr);
    CHECK(body.find("first line")  != std::string::npos);
    CHECK(body.find("second line") != std::string::npos);
    CHECK(body.find("non-text content: image (image/png)") != std::string::npos);
}

// ── 2. Subprocess wrapper ───────────────────────────────────────────

TEST_CASE("Subprocess: spawn /bin/cat and round-trip a line") {
    Subprocess proc({"/bin/cat"});
    REQUIRE(proc.alive());
    REQUIRE(proc.send_line("hello mcp"));

    auto got = proc.recv_line(2s);
    REQUIRE(got.has_value());
    CHECK(*got == "hello mcp");

    REQUIRE(proc.send_line("second"));
    got = proc.recv_line(2s);
    REQUIRE(got.has_value());
    CHECK(*got == "second");
}

TEST_CASE("Subprocess: recv_line returns nullopt on timeout") {
    Subprocess proc({"/bin/cat"});
    auto got = proc.recv_line(100ms);
    CHECK_FALSE(got.has_value());     // /bin/cat never spontaneously emits
}

TEST_CASE("Subprocess: terminate is idempotent and SIGKILLs after grace") {
    // /bin/sleep does not respond to stdin and ignores EOF on it, so the
    // SIGTERM-then-grace-then-SIGKILL escalation path is what reaps it.
    Subprocess proc({"/bin/sleep", "30"});
    CHECK(proc.alive());
    proc.terminate(50ms);
    CHECK_FALSE(proc.alive());
    proc.terminate(50ms);             // second call is a no-op
    CHECK_FALSE(proc.alive());
}

TEST_CASE("Subprocess: bad executable surfaces as immediate EOF") {
    // execvp failure inside the child _exits with 127; the parent sees
    // EOF on stdout and recv_line returns nullopt cleanly.
    Subprocess proc({"/nonexistent/bin/definitely_not_here"});
    auto got = proc.recv_line(500ms);
    CHECK_FALSE(got.has_value());
}

// ── 3. /mcp slash dispatch ─────────────────────────────────────────

TEST_CASE("parse_agent_commands recognises /mcp") {
    auto cmds = parse_agent_commands(
        "/mcp tools playwright\n"
        "/mcp call playwright browser_navigate {\"url\":\"https://example.com\"}\n"
    );
    REQUIRE(cmds.size() == 2);
    CHECK(cmds[0].name == "mcp");
    CHECK(cmds[0].args == "tools playwright");
    CHECK(cmds[1].name == "mcp");
    CHECK(cmds[1].args.find("browser_navigate") != std::string::npos);
}

TEST_CASE("/mcp dispatcher returns ERR when no invoker is wired") {
    auto cmds = parse_agent_commands("/mcp tools\n");
    auto out = execute_agent_commands(cmds, "test", "/tmp",
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, false, nullptr, nullptr, nullptr,
        /*mcp_invoker=*/nullptr);
    CHECK(out.find("[/mcp tools]") != std::string::npos);
    CHECK(out.find("ERR: MCP unavailable") != std::string::npos);
    CHECK(out.find("[END MCP]") != std::string::npos);
}

TEST_CASE("/mcp dispatcher routes 'tools' and 'call' to the invoker") {
    std::vector<std::pair<std::string,std::string>> seen;
    auto invoker = [&seen](const std::string& kind, const std::string& args) {
        seen.push_back({kind, args});
        if (kind == "tools") return std::string("[playwright]\n  navigate\n");
        if (kind == "call")  return std::string("OK: navigated to https://example.com\n");
        return std::string("ERR: huh\n");
    };

    auto cmds = parse_agent_commands(
        "/mcp tools\n"
        "/mcp call playwright browser_navigate {\"url\":\"https://example.com\"}\n"
    );
    auto out = execute_agent_commands(cmds, "test", "/tmp",
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, false, nullptr, nullptr, nullptr,
        /*mcp_invoker=*/invoker);

    REQUIRE(seen.size() == 2);
    CHECK(seen[0].first  == "tools");
    CHECK(seen[0].second == "");
    CHECK(seen[1].first  == "call");
    CHECK(seen[1].second.find("browser_navigate") != std::string::npos);

    CHECK(out.find("navigate")     != std::string::npos);
    CHECK(out.find("OK: navigated") != std::string::npos);
    CHECK(out.find("[END MCP]")    != std::string::npos);
}

TEST_CASE("/mcp rejects unknown subcommands without invoking") {
    bool invoked = false;
    auto invoker = [&invoked](const std::string&, const std::string&) {
        invoked = true;
        return std::string("should not be called");
    };
    auto cmds = parse_agent_commands("/mcp wat\n");
    auto out = execute_agent_commands(cmds, "test", "/tmp",
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, false, nullptr, nullptr, nullptr, invoker);
    CHECK_FALSE(invoked);
    CHECK(out.find("ERR: usage:") != std::string::npos);
}
