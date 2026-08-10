// End-to-end coverage for the /chat command family and sidebar
// rename/delete (Part 4.4/4.2). No live API calls (dummy key +
// ARBITER_OFFLINE via pty_harness) — turns fail locally after recording the
// user message, which is all these tests need.

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

// Poll until `token` shows up in bytes written after `offset`.  Commands
// queue FIFO behind any in-flight turn, so a generous budget doubles as
// the "wait for the previous turn" mechanism.
bool wait_for_token(PtySession& s, std::size_t offset, const std::string& token,
                    int budget_ms) {
    budget_ms = scale_timeout_ms(budget_ms);
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

TEST_CASE("/chat new + /chat switch <n> performs a full switch with replay in a narrow terminal") {
    // 80x24 narrowed to 60 cols: below HistorySidebarState::kMinCols (72),
    // so /chat is the only way to switch conversations here.
    PtySession s = ready_repl(24, 60);

    // Same race as transcript_replay_tui: switching while a turn is still
    // in flight hits the "Turn in progress — switch anyway?" gate and hangs.
    // Wait for the hermetic offline auth failure before /chat new|/chat switch.
    // Pane-edge clipping can drop the first glyph of a replayed line, so
    // assert on an interior substring (matches transcript_replay_tui).
    const std::string marker_probe = "conversation-marker";
    const std::size_t before_first = s.output().size();
    s.send("first-conversation-marker\r");
    REQUIRE(wait_for_token(s, before_first, marker_probe, 5000));
    REQUIRE(wait_for_token(s, before_first, "Authentication header", 5000));

    s.send("/chat new\r");
    s.read_for(1000);
    const std::size_t before_second = s.output().size();
    s.send("second-conversation-text\r");
    REQUIRE(wait_for_token(s, before_second, "Authentication header", 5000));

    s.send("/chat list\r");
    s.read_for(800);

    const std::string before = s.output();

    s.send("/chat switch 2\r");
    // Poll for replay rather than a fixed settle window — CI runners
    // (and sanitizer builds) routinely miss a single 1500ms read_for.
    REQUIRE(wait_for_token(s, before.size(), marker_probe, 15000));

    s.terminate();
}

TEST_CASE("sidebar rename (r) and soft delete (d + y)") {
    PtySession s = ready_repl(40, 100);

    // Seed the active conversation with a local title — no agent turn.  An
    // in-flight turn races sidebar focus on slow CI runners (keys land in
    // the editor instead of rename mode), which is why a fixed read_for
    // after `rename-me-marker\r` flakes on macos-arm64.
    const std::size_t before_seed = s.output().size();
    s.send("/chat title seed-title-for-rename\r");
    CHECK(wait_for_token(s, before_seed, "seed-title-for-rename", 10000));

    // Enter sidebar; pinned to the active entry (row 1) since only one
    // conversation exists.  Don't wait for "Conversations" in the delta —
    // OpenTUI may not re-emit unchanged header cells on focus.  Key on the
    // focused footer hint ("f folder") instead so `r` lands in rename mode
    // rather than the line editor on slow CI runners.
    const std::size_t before_focus = s.output().size();
    s.send("\x17" "b");
    REQUIRE(wait_for_token(s, before_focus, "fold", 10000));

    s.send("r");
    s.read_for(200);
    // Clear the pre-filled buffer before typing.
    for (int i = 0; i < 60; ++i) s.send("\x7f");
    s.read_for(200);

    const std::size_t before_rename = s.output().size();
    s.send("my-new-title");
    // While renaming, the buffer is painted into the row; poll until it
    // lands (CI runners are slower than a fixed 200ms read_for).
    CHECK(wait_for_token(s, before_rename, "my-new-title", 10000));
    s.send("\r");
    CHECK(wait_for_token(s, before_rename, "my-new-title", 10000));

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

TEST_CASE("/find reports match position in the status line and cycles") {
    PtySession s = ready_repl(24, 80);

    // Bare /find before any search prints usage.
    const std::string before_usage = s.output();
    s.send("/find\r");
    CHECK(wait_for_token(s, before_usage.size(), "cycle", 10000));

    // Seed ≥2 scrollback hits via hermetic offline turns (instant local
    // auth failure — no TLS). With a single hit, /find next leaves the
    // hit index unchanged.
    const std::size_t before_seed_a = s.output().size();
    s.send("seed-scrollback-aaa\r");
    REQUIRE(wait_for_token(s, before_seed_a, "seed-scrollback-aaa", 5000));
    REQUIRE(wait_for_token(s, before_seed_a, "Authentication header", 5000));
    const std::size_t before_seed_b = s.output().size();
    s.send("seed-scrollback-bbb\r");
    REQUIRE(wait_for_token(s, before_seed_b, "seed-scrollback-bbb", 5000));
    REQUIRE(wait_for_token(s, before_seed_b, "Authentication header", 5000));

    {
        // /help opens the interactive overlay menu; dismiss before typing.
        // Only the first page of rows is painted — wait for a top-of-list
        // token (not /find, which sits below the viewport).
        const std::size_t before_help = s.output().size();
        s.send("/help\r");
        REQUIRE(wait_for_token(s, before_help, "/send", 15000));
        s.send("\x1b");  // Esc — close menu, return stdin to the editor
        // Let the modal tear down before the next slash command.
        const std::size_t before_dismiss = s.output().size();
        (void)wait_for_token(s, before_dismiss, "esc interrupt", 3000);
    }

    const std::size_t before = s.output().size();
    s.send("/find scrollback\r");
    // First find jumps to the last hit (N/N) and paints @row into status.
    CHECK(wait_for_token(s, before, "\"scrollback\":", 10000));
    CHECK(wait_for_token(s, before, " @", 10000));

    // /find next clears then rewrites the whole status line (see
    // slash_commands.cpp), so the post-command delta contains a full
    // "1/N @row" paint — not a one-cell digit morph against the accumulated
    // stream.
    const std::size_t before_next = s.output().size();
    s.send("/find next\r");
    CHECK(wait_for_token(s, before_next, "\"scrollback\":", 15000));
    CHECK(wait_for_token(s, before_next, "1/", 15000));
    CHECK(wait_for_token(s, before_next, " @", 15000));

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
