// End-to-end coverage for the /chat command family and sidebar
// rename/delete (Part 4.4/4.2). No live API calls (dummy key, see
// pty_harness.h) — turns fail after the request but the user's message is
// still recorded in agent history beforehand, which is all these tests need.

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

TEST_CASE("/chat new + /chat switch <n> performs a full switch with replay in a narrow terminal") {
    // 80x24 narrowed to 60 cols: below HistorySidebarState::kMinCols (72),
    // so /chat is the only way to switch conversations here.
    PtySession s = ready_repl(24, 60);

    s.send("first-conversation-marker\r");
    s.read_for(1200);

    s.send("/chat new\r");
    s.read_for(1000);
    s.send("second-conversation-text\r");
    s.read_for(1200);

    s.send("/chat list\r");
    s.read_for(800);

    const std::string before = s.output();

    s.send("/chat switch 2\r");
    s.read_for(1500);

    REQUIRE(s.output().size() > before.size());
    const std::string new_bytes = PtySession::strip_ansi(s.output().substr(before.size()));
    CHECK(new_bytes.find("first-conversation-marker") != std::string::npos);

    s.terminate();
}

TEST_CASE("sidebar rename (r) and soft delete (d + y)") {
    PtySession s = ready_repl(40, 100);
    s.send("rename-me-marker\r");
    s.read_for(1200);

    // Enter sidebar; pinned to the active entry (row 1) since only one
    // conversation exists.
    s.send("\x17" "b");
    s.read_for(300);
    s.send("r");
    s.read_for(200);
    // Clear the pre-filled buffer (deterministic title) before typing.
    for (int i = 0; i < 60; ++i) s.send("\x7f");
    s.read_for(200);
    s.send("my-new-title");
    s.read_for(200);
    s.send("\r");
    s.read_for(500);
    CHECK(plain(s).find("my-new-title") != std::string::npos);

    // Delete it (still focused, still pinned to the same — only — entry).
    // Deleting the only conversation creates a fresh one, so the sidebar
    // must still render cleanly afterward.
    s.send("d");
    s.read_for(200);
    s.send("y");
    s.read_for(1500);
    CHECK(plain(s).find("Conversations") != std::string::npos);

    s.terminate();
}
