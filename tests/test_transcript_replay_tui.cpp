// End-to-end coverage for transcript replay (Part 3): switching back to an
// earlier conversation must re-render its transcript into the pane, not
// leave it blank. Runs the real binary under a PTY (no live API calls —
// the harness supplies a dummy key, so turns fail after the API call but
// the user's message is still recorded in agent history beforehand, which
// is all replay needs to prove it ran).

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

// Dummy OpenRouter key fails with an auth error once the turn completes.
// We can't wait on "ERR:" — the error style is clipped at the pane edge so
// the PTY stream shows "RR: …" instead. Slow CI runners need headroom
// (see test_line_editor.cpp).
void wait_turn_done(PtySession& s) {
    s.read_until("Authentication header", 20000);
}

// Poll until `needle` appears only in bytes written after `offset`.
void wait_for_new_bytes(PtySession& s, std::size_t offset, const std::string& needle, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        s.read_for(200);
        const std::string tail =
            PtySession::strip_ansi(s.output().substr(offset));
        if (tail.find(needle) != std::string::npos) return;
    }
    throw std::runtime_error("timeout waiting for new output containing '" + needle + "'");
}

} // namespace

TEST_CASE("switching back to an earlier conversation replays its transcript") {
    PtySession s = ready_repl(40, 100);

    const std::string marker = "first-conversation-marker-xyz";
    // Pane-edge clipping can eat the first glyph of a replayed line (same
    // artifact that turns "ERR:" into "RR:" in the PTY stream), so assert
    // on an interior substring rather than the full marker.
    const std::string marker_probe = "conversation-marker-xyz";
    s.send(marker + "\r");
    s.read_until(marker_probe, 5000);
    wait_turn_done(s);

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
    wait_turn_done(s);

    const std::string before_switch_back = s.output();

    // Enter the sidebar again (pins to the now-active second conversation,
    // the most-recently-updated entry) and move down to the older, first
    // conversation.
    s.send("\x17" "b");
    s.read_for(300);
    s.send("\033[B");
    s.read_for(200);
    s.send("\r");
    wait_for_new_bytes(s, before_switch_back.size(), marker_probe, 15000);

    REQUIRE(s.output().size() > before_switch_back.size());
    const std::string new_bytes =
        PtySession::strip_ansi(s.output().substr(before_switch_back.size()));

    CHECK(new_bytes.find(marker_probe) != std::string::npos);

    s.terminate();
}
