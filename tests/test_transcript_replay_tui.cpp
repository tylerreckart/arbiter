// End-to-end coverage for transcript replay (Part 3): switching back to an
// earlier conversation must re-render its transcript into the pane, not
// leave it blank. Runs the real binary under a PTY (no live API calls —
// the harness supplies a dummy key, so turns fail after the API call but
// the user's message is still recorded in agent history beforehand, which
// is all replay needs to prove it ran).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "pty_harness.h"

#include <string>

using namespace index_tests;

#ifndef INDEX_TEST_BINARY
#  error "INDEX_TEST_BINARY not defined"
#endif

namespace {

PtySession ready_repl(int rows, int cols) {
    PtySession s(rows, cols);
    s.spawn({ INDEX_TEST_BINARY });
    s.read_until("\033[?1049h", 10000);
    s.read_for(1500);
    return s;
}

std::string plain(const PtySession& s) {
    return PtySession::strip_ansi(s.output());
}

} // namespace

TEST_CASE("switching back to an earlier conversation replays its transcript") {
    PtySession s = ready_repl(40, 100);

    const std::string marker = "first-conversation-marker-xyz";
    s.send(marker + "\r");
    s.read_for(1500);
    REQUIRE(plain(s).find(marker) != std::string::npos);

    // Enter the history sidebar. With only one conversation, focus pins to
    // it (the active entry) rather than row 0 — move up once to land on
    // "+ New conversation", then create + switch to a second conversation.
    s.send("\x17" "b");
    s.read_for(300);
    s.send("\033[A");
    s.read_for(200);
    s.send("\r");
    s.read_for(800);

    s.send("second-conversation-text\r");
    s.read_for(1500);

    const std::string before_switch_back = s.output();

    // Enter the sidebar again (pins to the now-active second conversation,
    // the most-recently-updated entry) and move down to the older, first
    // conversation.
    s.send("\x17" "b");
    s.read_for(300);
    s.send("\033[B");
    s.read_for(200);
    s.send("\r");
    s.read_for(1500);

    REQUIRE(s.output().size() > before_switch_back.size());
    const std::string new_bytes =
        PtySession::strip_ansi(s.output().substr(before_switch_back.size()));

    CHECK(new_bytes.find(marker) != std::string::npos);

    s.terminate();
}
