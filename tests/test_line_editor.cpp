// LineEditor integration tests.  The editor owns stdin while blocking on
// user input and repaints the single input row on every keystroke, so most
// of its behavior is visible as a stream of "clear-line, prompt, buffer"
// repaints interleaved with cursor-positioning escapes.
//
// Assertions work on the ANSI-stripped output stream's *tail* — what the
// editor drew most recently.  For each test we send a sequence of bytes
// and check that either the final input-row content reflects the expected
// buffer state, or that a submitted line reached the scroll-region echo.
// (The stripped stream loses layout, but it preserves plain-text content
// and newlines, which is enough for what we're asserting.)

#include "doctest.h"
#include "pty_harness.h"

#include <string>
#include <algorithm>

using namespace index_tests;

#ifndef INDEX_TEST_BINARY
#  define INDEX_TEST_BINARY "/Users/tyler/dev/index/build/index"
#endif

// ANSI escape shorthands, so tests read like what the user would press.
static const std::string kArrUp    = "\033[A";
static const std::string kArrDown  = "\033[B";
static const std::string kArrRight = "\033[C";
static const std::string kArrLeft  = "\033[D";
static const std::string kHome     = "\033[H";
static const std::string kEnd      = "\033[F";

// Wait for the welcome card to settle, then drain whatever repaints happen
// during welcome → first-prompt transition.  After this returns, the editor
// is parked at an empty "> " prompt waiting for keystrokes.
static PtySession ready_editor(int rows = 40, int cols = 120) {
    PtySession s(rows, cols);
    s.spawn({ INDEX_TEST_BINARY });
    s.read_until("i'm index", 3000);
    s.read_for(400);
    return s;
}

// Grab the last N bytes of stripped output, so content assertions focus on
// the *current* input-row state rather than the full history of repaints.
static std::string tail_stripped(const PtySession& s, size_t bytes = 512) {
    std::string plain = PtySession::strip_ansi(s.output());
    if (plain.size() <= bytes) return plain;
    return plain.substr(plain.size() - bytes);
}

TEST_CASE("printable characters appear in the input row") {
    PtySession s = ready_editor();
    s.send("hello");
    s.read_for(300);

    // After typing "hello", the most recent input-row repaint ends with the
    // buffer contents.  The stripped tail should contain "hello" somewhere.
    CHECK(tail_stripped(s).find("hello") != std::string::npos);
}

TEST_CASE("backspace deletes the character before the cursor") {
    PtySession s = ready_editor();
    s.send("hellx");
    s.read_for(200);
    s.send("\x7F");      // DEL (backspace)
    s.send("o");
    s.read_for(300);

    std::string tail = tail_stripped(s);
    // Final buffer is "hello"; must not retain the typo "hellx".
    CHECK(tail.find("hello") != std::string::npos);
    // The mistyped variant should not be the *latest* rendered content.
    auto hellx_last = tail.rfind("hellx");
    auto hello_last = tail.rfind("hello");
    if (hellx_last != std::string::npos && hello_last != std::string::npos) {
        CHECK(hello_last > hellx_last);
    }
}

TEST_CASE("Ctrl-U kills the whole input line") {
    PtySession s = ready_editor();
    s.send("garbage");
    s.read_for(200);
    s.send("\x15");      // ^U
    s.read_for(200);
    s.send("ok");
    s.read_for(300);

    // After ^U the buffer cleared, so "garbage" shouldn't be the most
    // recent content — "ok" should be.
    std::string tail = tail_stripped(s);
    auto garbage_last = tail.rfind("garbage");
    auto ok_last      = tail.rfind("ok");
    REQUIRE(ok_last != std::string::npos);
    if (garbage_last != std::string::npos) {
        CHECK(ok_last > garbage_last);
    }
}

TEST_CASE("Ctrl-W kills the previous word") {
    PtySession s = ready_editor();
    s.send("hello world");
    s.read_for(200);
    s.send("\x17");      // ^W
    s.read_for(300);

    // After ^W, "world" is gone — the latest repaint shows "hello ".
    std::string tail = tail_stripped(s);
    auto hello_last = tail.rfind("hello");
    auto world_last = tail.rfind("world");
    REQUIRE(hello_last != std::string::npos);
    if (world_last != std::string::npos) {
        CHECK(hello_last > world_last);
    }
}

TEST_CASE("history: up arrow recalls previous submission") {
    PtySession s = ready_editor();

    // Submit a first line.  With a dummy API key the send will surface an
    // error, but the line is nonetheless pushed into editor history.
    s.send("first\r");
    s.read_for(1200);     // let the dismiss + echo + API failure settle

    // Up arrow — should repaint the input row with "first" in the buffer.
    s.send(kArrUp);
    s.read_for(400);

    // "first" must appear near the end of the stripped stream (the recall
    // repaint).  It will also appear earlier as the prompt echo; we just
    // need any occurrence in the tail to confirm recall painted it.
    CHECK(tail_stripped(s).find("first") != std::string::npos);
}

TEST_CASE("multi-line continuation: trailing backslash defers submission") {
    PtySession s = ready_editor();

    // First fragment ends with a backslash — the editor returns the line to
    // the REPL, which accumulates it and re-enters read_line with a
    // continuation prompt ("…").  Nothing should be dispatched yet.
    s.send("first\\\r");
    s.read_for(400);

    // The continuation prompt glyph (U+2026 = e2 80 a6) should appear after
    // the backslash-terminated line is accepted.
    CHECK(s.output().find("\xE2\x80\xA6") != std::string::npos);

    // Second fragment — submitting it should push the combined line through
    // the echo path.  Echo uses ANSI colors so check the stripped view.
    s.send("second\r");
    s.read_for(1500);

    std::string plain = PtySession::strip_ansi(s.output());
    CHECK(plain.find("first") != std::string::npos);
    CHECK(plain.find("second") != std::string::npos);
}

TEST_CASE("Ctrl-D on empty buffer exits cleanly") {
    PtySession s = ready_editor();
    s.send("\x04");       // ^D on empty input → EOF
    s.read_for(1000);

    // After EOF the REPL exits; the binary closes the alt-screen (\033[?1049l)
    // and the PTY master eventually sees EOF.  A quick way to verify: try to
    // send more input — if the child is gone the write still "succeeds" to
    // the PTY buffer, so instead check the alt-screen-leave sequence appears.
    CHECK(s.output().find("\033[?1049l") != std::string::npos);
}

TEST_CASE("ESC clears an in-progress line without submitting") {
    PtySession s = ready_editor();

    s.send("willbe-cancelled");
    s.read_for(200);
    s.send("\033");       // lone ESC → cancel path after ~50ms timeout
    s.read_for(300);      // give the editor's CSI-timeout code time to fire

    // Type a replacement so we can assert order — "willbe-cancelled" must
    // have been rendered earlier than "after-cancel".
    s.send("after-cancel");
    s.read_for(300);

    std::string tail = tail_stripped(s);
    auto cancelled_last = tail.rfind("willbe-cancelled");
    auto after_last     = tail.rfind("after-cancel");
    REQUIRE(after_last != std::string::npos);
    if (cancelled_last != std::string::npos) {
        CHECK(after_last > cancelled_last);
    }
}

TEST_CASE("Home and End move the cursor to the buffer extremes") {
    PtySession s = ready_editor();
    s.send("abcdef");
    s.read_for(200);
    s.send(kHome);
    s.send("X");
    s.read_for(200);
    s.send(kEnd);
    s.send("Y");
    s.read_for(300);

    // Final buffer should be "Xabcdef" + "Y" = "XabcdefY".
    CHECK(tail_stripped(s).find("XabcdefY") != std::string::npos);
}

TEST_CASE("left/right arrow cursor navigation allows mid-string insertion") {
    PtySession s = ready_editor();
    s.send("helo");
    s.read_for(200);
    // Cursor is past 'o'; move left once to land before 'o', insert 'l'.
    s.send(kArrLeft);
    s.send("l");
    s.read_for(300);

    // Buffer is now "hello" — the classic typo-fix motion.
    CHECK(tail_stripped(s).find("hello") != std::string::npos);
}
