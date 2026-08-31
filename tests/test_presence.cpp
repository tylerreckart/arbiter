// tests/test_presence.cpp — Parser, watch matching, injection framing,
// and input caps for always-on presence.  Runtime injection (orchestrator
// tool-result prepend) needs a live or mocked ApiClient and lives outside
// this target.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "presence.h"

using namespace arbiter;

TEST_CASE("parse: bare SILENT") {
    auto out = parse_presence_signal("<signal>SILENT</signal>");
    CHECK(out.kind == PresenceOutput::Kind::Silent);
    CHECK(out.malformed == false);
    CHECK(out.text.empty());
}

TEST_CASE("parse: SILENT tolerates surrounding prose") {
    auto out = parse_presence_signal(
        "looks fine\n<signal>SILENT</signal>\ntrailing\n");
    CHECK(out.kind == PresenceOutput::Kind::Silent);
    CHECK(out.malformed == false);
}

TEST_CASE("parse: CONTEXT extracts note body") {
    auto out = parse_presence_signal(
        "<signal>CONTEXT</signal>\n"
        "<note>The env dump in the last /exec contains a token.</note>");
    CHECK(out.kind == PresenceOutput::Kind::Context);
    CHECK(out.text == "The env dump in the last /exec contains a token.");
    CHECK(out.malformed == false);
}

TEST_CASE("parse: CONTEXT without note is malformed Silent") {
    auto out = parse_presence_signal("<signal>CONTEXT</signal>");
    CHECK(out.kind == PresenceOutput::Kind::Silent);
    CHECK(out.malformed == true);
}

TEST_CASE("parse: signal token is case-insensitive; body keeps casing") {
    auto a = parse_presence_signal("<signal>silent</signal>");
    CHECK(a.kind == PresenceOutput::Kind::Silent);
    CHECK(a.malformed == false);

    auto b = parse_presence_signal(
        "<signal>Context</signal><note>Use HTTPS, NOT http.</note>");
    CHECK(b.kind == PresenceOutput::Kind::Context);
    CHECK(b.text == "Use HTTPS, NOT http.");
}

TEST_CASE("parse: missing signal is malformed Silent") {
    auto out = parse_presence_signal("CONTEXT — you should know about X");
    CHECK(out.kind == PresenceOutput::Kind::Silent);
    CHECK(out.malformed == true);
    CHECK(out.raw == "CONTEXT — you should know about X");
}

TEST_CASE("parse: unrecognised signal is malformed Silent") {
    auto out = parse_presence_signal("<signal>HALT</signal><reason>no</reason>");
    CHECK(out.kind == PresenceOutput::Kind::Silent);
    CHECK(out.malformed == true);
}

TEST_CASE("parse: empty input is malformed Silent") {
    auto out = parse_presence_signal("");
    CHECK(out.kind == PresenceOutput::Kind::Silent);
    CHECK(out.malformed == true);
}

TEST_CASE("parse: note whitespace is trimmed") {
    auto out = parse_presence_signal(
        "<signal>CONTEXT</signal>\n"
        "<note>\n  leading and trailing  \n</note>");
    CHECK(out.text == "leading and trailing");
}

TEST_CASE("watch: empty watch matches every non-empty id") {
    PresenceConfig cfg;
    CHECK(presence_watch_matches(cfg, "index"));
    CHECK(presence_watch_matches(cfg, "forge"));
    CHECK_FALSE(presence_watch_matches(cfg, ""));
}

TEST_CASE("watch: exact and glob patterns") {
    PresenceConfig cfg;
    cfg.watch = {"index", "forge*"};
    CHECK(presence_watch_matches(cfg, "index"));
    CHECK(presence_watch_matches(cfg, "forge"));
    CHECK(presence_watch_matches(cfg, "forge-ci"));
    CHECK_FALSE(presence_watch_matches(cfg, "scout"));
}

TEST_CASE("watch: star matches all") {
    PresenceConfig cfg;
    cfg.watch = {"*"};
    CHECK(presence_watch_matches(cfg, "index"));
    CHECK(presence_watch_matches(cfg, "anchor"));
}

TEST_CASE("active: always_on + context is active; off is not") {
    PresenceConfig cfg;
    CHECK_FALSE(presence_is_active(cfg));
    cfg.mode = "always_on";
    CHECK(presence_is_active(cfg));
    cfg.interject = "off";
    CHECK_FALSE(presence_is_active(cfg));
}

TEST_CASE("format: injection block uses watcher name") {
    auto block = format_presence_injection(
        "Anchor", "Memory #47 already pinned the sandbox path.");
    CHECK(block.find("[PRESENCE: Anchor]\n") == 0);
    CHECK(block.find("Memory #47 already pinned the sandbox path.") != std::string::npos);
    CHECK(block.find("[END PRESENCE]\n\n") != std::string::npos);
}

TEST_CASE("format: empty name falls back to presence") {
    auto block = format_presence_injection("", "note");
    CHECK(block.find("[PRESENCE: presence]\n") == 0);
}

TEST_CASE("cap: oversized fields truncate with marker") {
    PresenceInput in;
    in.original_task.assign(kPresenceMaxOriginalTask + 80, 'x');
    in.recent_text.assign(kPresenceMaxRecentText + 80, 'y');
    in.tool_summary.assign(kPresenceMaxToolSummary + 80, 'z');
    cap_presence_input(in);
    CHECK(in.original_task.size() <= kPresenceMaxOriginalTask);
    CHECK(in.recent_text.size() <= kPresenceMaxRecentText);
    CHECK(in.tool_summary.size() <= kPresenceMaxToolSummary);
    CHECK(in.original_task.find("[truncated]") != std::string::npos);
    const auto once = in.original_task;
    cap_presence_input(in);
    CHECK(in.original_task == once);
}

TEST_CASE("cap: prompt override truncates") {
    std::string prompt(kPresenceMaxPromptOverride + 40, 'p');
    auto capped = cap_presence_prompt_override(prompt);
    CHECK(capped.size() <= kPresenceMaxPromptOverride);
    CHECK(capped.find("[truncated]") != std::string::npos);
}

TEST_CASE("presence_model falls back to the watcher's model") {
    PresenceConfig cfg;
    CHECK(presence_model(cfg, "anthropic/claude-haiku-4-5") ==
          "anthropic/claude-haiku-4-5");
    cfg.model = "anthropic/claude-sonnet-4-6";
    CHECK(presence_model(cfg, "anthropic/claude-haiku-4-5") ==
          "anthropic/claude-sonnet-4-6");
}
