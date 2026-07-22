// Sidebar visibility in the live TUI (PTY).  Session sidebar width is based
// on remaining columns after the history sidebar (26 + 1 gutter).  Below the
// 96-col remaining breakpoint it must not paint section labels; above it may
// after the first prompt.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "pty_harness.h"

#include <chrono>
#include <string>

using namespace index_tests;

#ifndef INDEX_TEST_BINARY
#  error "INDEX_TEST_BINARY not defined"
#endif

static PtySession ready_repl(int rows, int cols) {
    PtySession s(rows, cols);
    s.spawn({ INDEX_TEST_BINARY });
    s.read_until("\033[?1049h", 10000);
    s.read_for(1500);
    return s;
}

static std::string plain(const PtySession& s) {
    return PtySession::strip_ansi(s.output());
}

static bool wait_for_plain(PtySession& s, const std::string& token, int budget_ms) {
    budget_ms = scale_timeout_ms(budget_ms);
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(budget_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        s.read_for(50);
        if (PtySession::strip_ansi(s.output()).find(token) != std::string::npos)
            return true;
    }
    return false;
}

TEST_CASE("sidebar section labels hidden below 96 columns after first prompt") {
    PtySession s = ready_repl(40, 80);
    s.send("hello\r");
    s.read_for(1500);
    const std::string out = plain(s);
    CHECK(out.find("Context") == std::string::npos);
    CHECK(out.find("(none yet)") == std::string::npos);
    s.terminate();
}

TEST_CASE("sidebar section labels appear when remaining width clears 96 cols") {
    // History sidebar reserves 27 cols (26 box + outer gutter).  147 leaves
    // 120 remaining → 28-col session sidebar (see SidebarState::breakpoint_width).
    PtySession s = ready_repl(40, 147);
    s.send("hello\r");
    CHECK(wait_for_plain(s, "Context", 15000));
    s.terminate();
}
