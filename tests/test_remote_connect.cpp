// tests/test_remote_connect.cpp — URL/token resolution + SSE turn consumer.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "a2a/sse_reader.h"
#include "remote/connect_config.h"
#include "remote/sse_turn.h"
#include "render_policy.h"
#include "repl/queues.h"
#include "repl/repl_argv.h"
#include "stream_renderer.h"

#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>

using namespace arbiter;

TEST_CASE("normalize_api_base_url rejects missing scheme and empty host") {
    CHECK(normalize_api_base_url("").empty());
    CHECK(normalize_api_base_url("  ").empty());
    CHECK(normalize_api_base_url("arbiter.example.com").empty());
    CHECK(normalize_api_base_url("ftp://x").empty());
    CHECK(normalize_api_base_url("http://").empty());
    CHECK(normalize_api_base_url("https://").empty());
}

TEST_CASE("normalize_api_base_url strips trailing slashes") {
    CHECK(normalize_api_base_url("https://arbiter.example.com/") ==
          "https://arbiter.example.com");
    CHECK(normalize_api_base_url("http://127.0.0.1:8080///") ==
          "http://127.0.0.1:8080");
}

TEST_CASE("api_display_host extracts host:port") {
    CHECK(api_display_host("https://arbiter.example.com") == "arbiter.example.com");
    CHECK(api_display_host("http://127.0.0.1:8080") == "127.0.0.1:8080");
}

TEST_CASE("resolve_remote_connect requires URL") {
    RemoteConnectConfig cfg;
    const std::string err = resolve_remote_connect(cfg);
    CHECK_FALSE(err.empty());
}

TEST_CASE("resolve_remote_connect accepts URL without token") {
    RemoteConnectConfig cfg;
    cfg.base_url = "https://arbiter.example.com/";
    CHECK(resolve_remote_connect(cfg).empty());
    CHECK(cfg.base_url == "https://arbiter.example.com");
    CHECK(cfg.display_host == "arbiter.example.com");
    CHECK(cfg.token.empty());
}

TEST_CASE("resolve_remote_connect reads env fallbacks") {
    RemoteConnectConfig cfg;
    ::setenv("ARBITER_API_URL", "http://localhost:9090", 1);
    ::setenv("ARBITER_API_TOKEN", "atr_test_token", 1);
    CHECK(resolve_remote_connect(cfg).empty());
    CHECK(cfg.base_url == "http://localhost:9090");
    CHECK(cfg.token == "atr_test_token");
    ::unsetenv("ARBITER_API_URL");
    ::unsetenv("ARBITER_API_TOKEN");
}

TEST_CASE("parse_connect_argv picks --connect and --token") {
    const char* argv[] = {
        "arbiter", "--connect", "https://h.example", "--token", "atr_x", nullptr
    };
    auto cfg = parse_connect_argv(5, const_cast<char**>(argv));
    CHECK(cfg.base_url == "https://h.example");
    CHECK(cfg.token == "atr_x");
    CHECK(argv_has_connect(5, const_cast<char**>(argv)));
}

TEST_CASE("parse_connect_argv allows --connect without URL") {
    const char* argv[] = {"arbiter", "--connect", "--theme", "nord", nullptr};
    auto cfg = parse_connect_argv(4, const_cast<char**>(argv));
    CHECK(cfg.base_url.empty());
    CHECK(argv_has_connect(4, const_cast<char**>(argv)));
    CHECK(argv_launches_interactive(4, const_cast<char**>(argv)));
}

TEST_CASE("RemoteSseTurnConsumer feeds text deltas and done") {
    OutputQueue queue;
    StreamRenderer renderer(kMasterStream, queue);
    RemoteSseTurnConsumer consumer(renderer, queue);

    consumer.on_event("request_received",
        R"({"request_id":"abc123","tenant":"default","tenant_id":1,"message":"hi"})");
    consumer.on_event("text", R"({"agent":"index","stream_id":1,"depth":0,"delta":"Hello"})");
    consumer.on_event("text", R"({"agent":"index","stream_id":1,"depth":0,"delta":" world"})");
    consumer.on_event("done",
        R"({"ok":true,"content":"Hello world","input_tokens":10,"output_tokens":2,"request_id":"abc123"})");

    auto result = consumer.finish(false);
    CHECK(result.ok);
    CHECK(result.request_id == "abc123");
    CHECK(result.content == "Hello world");
    CHECK(result.input_tokens == 10);
    CHECK(result.output_tokens == 2);
    CHECK(result.error.empty());
}

TEST_CASE("RemoteSseTurnConsumer surfaces done.ok=false") {
    OutputQueue queue;
    StreamRenderer renderer(kMasterStream, queue);
    RemoteSseTurnConsumer consumer(renderer, queue);
    consumer.on_event("done", R"({"ok":false,"error":"upstream failed","content":""})");
    auto result = consumer.finish(false);
    CHECK_FALSE(result.ok);
    CHECK(result.error == "upstream failed");
}

TEST_CASE("RemoteSseTurnConsumer cancel without done") {
    OutputQueue queue;
    StreamRenderer renderer(kMasterStream, queue);
    RemoteSseTurnConsumer consumer(renderer, queue);
    consumer.on_event("request_received", R"({"request_id":"r1"})");
    consumer.on_event("text", R"({"delta":"partial"})");
    auto result = consumer.finish(true);
    CHECK_FALSE(result.ok);
    CHECK(result.cancelled);
    CHECK(result.error == "interrupted");
    CHECK(result.request_id == "r1");
}

TEST_CASE("RemoteSseTurnConsumer captures request_id via hook") {
    OutputQueue queue;
    StreamRenderer renderer(kMasterStream, queue);
    std::string seen;
    RemoteTurnHooks hooks;
    hooks.on_request_id = [&](const std::string& id) { seen = id; };
    RemoteSseTurnConsumer consumer(renderer, queue, hooks);
    consumer.on_event("request_received", R"({"request_id":"hooked"})");
    CHECK(seen == "hooked");
}

TEST_CASE("SseReader pipes into RemoteSseTurnConsumer") {
    OutputQueue queue;
    StreamRenderer renderer(kMasterStream, queue);
    RemoteSseTurnConsumer consumer(renderer, queue);
    a2a::SseReader reader([&](const std::string& name, const std::string& data) {
        consumer.on_event(name, data);
    });

    const std::string chunk =
        "event: request_received\n"
        "data: {\"request_id\":\"sse1\"}\n"
        "\n"
        "event: text\n"
        "data: {\"delta\":\"ok\"}\n"
        "\n"
        "event: done\n"
        "data: {\"ok\":true,\"content\":\"ok\",\"input_tokens\":1,\"output_tokens\":1}\n"
        "\n";
    reader.feed(chunk.data(), chunk.size());
    reader.flush(true);

    auto result = consumer.finish(false);
    CHECK(result.ok);
    CHECK(result.request_id == "sse1");
    CHECK(result.content == "ok");
}
