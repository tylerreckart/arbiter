// PTY regression: operational logs emitted on a turn thread must not land
// in the frame, and a one-agent Fleet box must stay content-sized.
//
// ARBITER_TUI_LOG_PROBE writes the openrouter_response fprintf plus
// intent_route / intent_shadow Logger lines from handle_line. Those are
// the lines that used to interleave with the readline band (the `[7nfo]`
// corruption). ARBITER_TUI_LAYOUT_TRACE records the painted rail split
// on the same stderr, which the TUI redirects to ~/.arbiter/tui.log.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "pty_harness.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

using namespace index_tests;

#ifndef INDEX_TEST_BINARY
#  error "INDEX_TEST_BINARY not defined"
#endif

namespace {

bool wait_for_raw(PtySession& s, std::size_t offset, const std::string& token,
                  int budget_ms) {
    budget_ms = scale_timeout_ms(budget_ms);
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(budget_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        s.read_for(150);
        if (s.output().substr(offset).find(token) != std::string::npos) return true;
    }
    return false;
}

// The live input row is an OpenTUI cell diff. A block cursor can sit
// between the typed characters, so the buffer is not one contiguous ASCII
// run after ANSI stripping (same split as tests/test_line_editor.cpp).
// Dropping non-ASCII joins a UTF-8 cursor cell back out.
std::string collapse_ascii(const std::string& plain) {
    std::string ascii;
    ascii.reserve(plain.size());
    for (unsigned char c : plain) {
        if (c < 0x80) ascii.push_back(static_cast<char>(c));
    }
    return ascii;
}

bool plain_tail_has(PtySession& s, std::size_t offset, const std::string& token) {
    if (s.output().size() <= offset) return false;
    const std::string tail = PtySession::strip_ansi(s.output().substr(offset));
    if (tail.find(token) != std::string::npos) return true;
    return collapse_ascii(tail).find(token) != std::string::npos;
}

bool wait_for_plain_after(PtySession& s, std::size_t offset, const std::string& token,
                          int budget_ms) {
    budget_ms = scale_timeout_ms(budget_ms);
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(budget_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        s.read_for(150);
        if (plain_tail_has(s, offset, token)) return true;
    }
    return false;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool wait_for_log(PtySession& s, const std::string& token, int budget_ms) {
    const std::string path = s.home() + "/.arbiter/tui.log";
    budget_ms = scale_timeout_ms(budget_ms);
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(budget_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (read_file(path).find(token) != std::string::npos) return true;
        s.read_for(100);
    }
    return false;
}

}  // namespace

TEST_CASE("turn logs stay off the input line and fleet sizes to one agent") {
    // 160 cols: history sidebar (27) leaves >=120, so both rails are up
    // after the first prompt. 40 rows used to give Fleet max(10, 16).
    PtySession s(40, 160);
    s.env("ARBITER_TUI_LOG_PROBE", "1");
    s.env("ARBITER_TUI_LAYOUT_TRACE", "1");
    s.spawn({ INDEX_TEST_BINARY });
    s.read_until("\033[?1049h", 10000);

    // Idle: no job, so the fleet rect is absent (height 0).
    REQUIRE(wait_for_log(s, "tui_layout rows=40 cols=160 fleet_h=0 fleet_rows=0", 10000));

    const std::size_t before = s.output().size();
    s.send("probe-turn-marker\r");
    REQUIRE(wait_for_plain_after(s, before, "Authentication header", 15000));

    // Fleet gaining its first row narrows the pane. That resize aborts
    // read_line, and keystrokes already pulled into the aborted line are
    // discarded. On macOS that lands after "Authentication header" is
    // visible, so a single type burst never shows up. Wait until the
    // resized frame has been logged, then type again if a late interrupt
    // still eats the first attempt.
    REQUIRE(wait_for_log(s,
                         "fleet_h=5 fleet_rows=1 session_y=5 session_h=35",
                         10000));
    s.read_for(400);

    const std::size_t before_type = s.output().size();
    bool input_ok = false;
    for (int attempt = 0; attempt < 4 && !input_ok; ++attempt) {
        s.send("\x15");  // Ctrl-U: drop a partial line from a raced attempt
        s.read_for(50);
        s.send("clean-input-zz");
        input_ok = wait_for_plain_after(s, before_type, "clean-input-zz", 2500);
    }
    if (!input_ok) {
        s.send("\r");
        input_ok = wait_for_plain_after(s, before_type, "clean-input-zz", 8000);
    }
    if (!input_ok) {
        std::string tail = s.output().size() > before_type
            ? collapse_ascii(PtySession::strip_ansi(s.output().substr(before_type)))
            : std::string{};
        if (tail.size() > 600) tail = tail.substr(tail.size() - 600);
        REQUIRE_MESSAGE(input_ok, "plain tail after type: " << tail);
    }
    REQUIRE(input_ok);

    const std::string raw = s.output();
    CHECK(raw.find("openrouter_response") == std::string::npos);
    CHECK(raw.find("intent_route") == std::string::npos);
    CHECK(raw.find("intent_shadow") == std::string::npos);
    CHECK(raw.find("tui_layout") == std::string::npos);
    CHECK(raw.find("[7nfo]") == std::string::npos);
    CHECK(raw.find("cost_usd=0.001197") == std::string::npos);

    const std::string log_path = s.home() + "/.arbiter/tui.log";
    REQUIRE(wait_for_log(s,
                         "fleet_h=5 fleet_rows=1 session_y=5 session_h=35",
                         10000));
    const std::string logged = read_file(log_path);
    CHECK(logged.find("[info] openrouter_response requested_model=anthropic/claude-haiku-4-5 "
                      "served_model=anthropic/claude-haiku-4.5 cost_usd=0.001197")
          != std::string::npos);
    CHECK(logged.find("intent_route") != std::string::npos);
    CHECK(logged.find("tier=probe") != std::string::npos);
    CHECK(logged.find("intent_shadow") != std::string::npos);
    CHECK(logged.find("backend=probe") != std::string::npos);
    // One agent must not reclaim the old 2/5 split (16 rows on this tty).
    CHECK(logged.find("fleet_h=16") == std::string::npos);

    s.terminate();
}
