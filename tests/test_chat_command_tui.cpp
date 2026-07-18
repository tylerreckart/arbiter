// End-to-end coverage for the /chat command family and sidebar
// rename/delete (Part 4.4/4.2). No live API calls (dummy key, see
// pty_harness.h) — turns fail after the request but the user's message is
// still recorded in agent history beforehand, which is all these tests need.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "pty_harness.h"

#include <chrono>
#include <stdexcept>
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

namespace {

// Poll until `token` shows up in bytes written after `offset`.  Commands
// queue FIFO behind any in-flight turn, so a generous budget doubles as
// the "wait for the previous turn" mechanism.
bool wait_for_token(PtySession& s, std::size_t offset, const std::string& token,
                    int budget_ms) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(budget_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        s.read_for(200);
        const std::string tail = PtySession::strip_ansi(s.output().substr(offset));
        if (tail.find(token) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST_CASE("/find reports match position in the status line and cycles") {
    PtySession s = ready_repl(24, 80);

    // Bare /find before any search prints usage.
    const std::string before_usage = s.output();
    s.send("/find\r");
    CHECK(wait_for_token(s, before_usage.size(), "cycle", 10000));

    // /help renders a large block into scrollback with no agent turn — a
    // network-free search corpus ("scrollback" appears in its source).
    // Assertions match contiguous tokens only: the framebuffer renderer
    // can drop spaces between draw runs in the PTY stream.
    s.send("/help\r");
    s.read_for(800);

    const std::string before = s.output();
    s.send("/find scrollback\r");
    CHECK(wait_for_token(s, before.size(), "\"scrollback\":", 10000));

    const std::string before_step = s.output();
    s.send("/find next\r");
    CHECK(wait_for_token(s, before_step.size(), "\"scrollback\":", 10000));

    s.terminate();
}

TEST_CASE("/chat search matches a conversation title") {
    PtySession s = ready_repl(24, 80);

    // Titling needs no agent turn (unlike transcript content, which is only
    // saved when a turn completes), so this stays network-free.
    s.send("/chat title fluxcap planning notes\r");
    s.read_for(600);

    const std::string before = s.output();
    s.send("/chat search fluxcap\r");
    // A hit line ends "(N match)" / "(N matches)"; the no-hit message ends
    // with the quoted term instead.
    CHECK(wait_for_token(s, before.size(), "match)", 10000));
    CHECK(wait_for_token(s, before.size(), "id-prefix", 5000));

    s.terminate();
}
