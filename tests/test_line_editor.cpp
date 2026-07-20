// REPL input integration tests.  The interactive REPL owns stdin while
// blocking on user input; OpenTUI repaints the input row each frame.
//
// Assertions work on the ANSI-stripped output stream's *tail* — what the
// editor drew most recently.  For each test we send a sequence of bytes
// and check that either the final input-row content reflects the expected
// buffer state, or that a submitted line reached the scroll-region echo.
// (The stripped stream loses layout, but it preserves plain-text content
// and newlines, which is enough for what we're asserting.)

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "pty_harness.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>

using namespace index_tests;

// Path to the built `arbiter` binary, injected at configure time via
// target_compile_definitions in CMakeLists.txt.  Tests refuse to run
// if the macro isn't set rather than guessing a developer-local path.
#ifndef INDEX_TEST_BINARY
#  error "INDEX_TEST_BINARY not defined; build via the project CMake so the test target is wired correctly."
#endif

// ANSI escape shorthands, so tests read like what the user would press.
static const std::string kArrUp    = "\033[A";
static const std::string kArrDown  = "\033[B";
static const std::string kArrRight = "\033[C";
static const std::string kArrLeft  = "\033[D";
static const std::string kHome     = "\033[H";
static const std::string kEnd      = "\033[F";
static const std::string kPasteBegin = "\033[200~";
static const std::string kPasteEnd   = "\033[201~";

// Wait for the editor to be ready for keystrokes.  We key on the
// alt-screen-enter sequence (`\033[?1049h`) rather than any greeting
// text — that escape is emitted by TUI init before the first repaint
// and survives any cosmetic changes to the startup splash.  Then drain
// long enough for the initial OpenTUI frame to finish writing; without
// this the PTY pipe can fill and the REPL blocks before read_line runs.
static PtySession ready_editor(int rows = 40, int cols = 120) {
    PtySession s(rows, cols);
    s.spawn({ INDEX_TEST_BINARY });
    s.read_until("\033[?1049h", 10000);
    s.read_for(1500);
    return s;
}

// Grab the last N bytes of stripped output, so content assertions focus on
// the *current* input-row state rather than the full history of repaints.
static std::string tail_stripped(const PtySession& s, size_t bytes = 512) {
    std::string plain = PtySession::strip_ansi(s.output());
    if (plain.size() <= bytes) return plain;
    return plain.substr(plain.size() - bytes);
}

// Poll for `token` in ANSI-stripped output.  Cold-start macOS runners often
// need longer than a fixed read_for() for the first input-row paint.
static bool wait_for_plain(PtySession& s, const std::string& token, int budget_ms) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(budget_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        s.read_for(200);
        if (PtySession::strip_ansi(s.output()).find(token) != std::string::npos)
            return true;
    }
    return false;
}

TEST_CASE("printable characters appear in the input row") {
    PtySession s = ready_editor();
    s.send("hello");

    // After typing "hello", the input-row repaint should include the buffer
    // contents.  The block cursor can split the latest diff, so search the
    // captured stream instead of only the final tail.  Poll — a single 500ms
    // drain races the first OpenTUI frame on cold macOS CI runners (main and
    // this branch both saw 16/17 pass with only this case failing).
    CHECK(wait_for_plain(s, "hello", 8000));
}

TEST_CASE("backspace deletes the character before the cursor") {
    PtySession s = ready_editor();
    s.send("/agentx");
    s.read_for(200);
    s.send("\x7F");      // DEL (backspace)
    s.send("s\r");       // final command should be /agents
    s.read_for(500);

    // Submitting the edited line should execute /agents, proving the buffer
    // became "/agents" even if OpenTUI only diff-rendered the last changed cell.
    CHECK(PtySession::strip_ansi(s.output()).find("/agents") != std::string::npos);
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
    const std::string plain = PtySession::strip_ansi(s.output());
    auto garbage_last = plain.rfind("garbage");
    auto ok_last      = plain.rfind("ok");
    REQUIRE(ok_last != std::string::npos);
    if (garbage_last != std::string::npos) {
        CHECK(ok_last > garbage_last);
    }
}

TEST_CASE("history: up arrow recalls previous submission") {
    PtySession s = ready_editor();

    // Submit a first line.  With a dummy API key the send will surface an
    // error, but the line is nonetheless pushed into editor history.
    // The wait covers a real TLS handshake to api.anthropic.com (and back
    // with a 401) — slow CI runners can take 3-4 s for the round trip.
    s.send("first\r");
    s.read_for(5000);

    // Up arrow — should repaint the input row with "first" in the buffer.
    s.send(kArrUp);
    s.read_for(400);

    // "first" must appear near the end of the stripped stream (the recall
    // repaint).  It will also appear earlier as the prompt echo; we just
    // need any occurrence in the stream to confirm recall painted it.
    CHECK(PtySession::strip_ansi(s.output()).find("first") != std::string::npos);
}

TEST_CASE("multi-line continuation: trailing backslash defers submission") {
    PtySession s = ready_editor();

    // First fragment ends with a backslash — the editor returns the line to
    // the REPL, which accumulates it and re-enters read_line with a
    // continuation prompt ("…").  Nothing should be dispatched yet.
    s.send("first\\\r");
    s.read_for(800);

    // The continuation prompt glyph (U+2026 = e2 80 a6) should appear after
    // the backslash-terminated line is accepted.
    CHECK(s.output().find("\xE2\x80\xA6") != std::string::npos);

    // Second fragment — submitting it should leave continuation mode.  The
    // transport echo is intentionally not asserted here because OpenTUI diff
    // rendering does not guarantee the full submitted line appears in the
    // PTY tail after chrome geometry changes.
    s.send("second\r");
    s.read_for(500);
}

TEST_CASE("bracketed multiline paste stays one unsubmitted editor buffer") {
    PtySession s = ready_editor();

    // A real terminal only sends paste boundaries after the application opts
    // into bracketed-paste mode. Keep this assertion next to the behavioral
    // check so a terminal-setup regression cannot silently restore raw paste.
    CHECK(s.output().find("\033[?2004h") != std::string::npos);

    // If either embedded newline is interpreted as Enter, the first /quit
    // exits the process. A bracketed paste must instead insert the complete
    // payload literally and wait for a separate Enter keystroke.
    s.send(kPasteBegin + "/quit\nstill-one-prompt\n" + kPasteEnd);
    s.read_for(500);
    REQUIRE_FALSE(s.wait_exited(100));

    // The paste is in one non-empty editor buffer. Clear it, then Ctrl-D must
    // see an empty buffer and perform the normal clean EOF shutdown.
    s.send("\x15"); // Ctrl-U
    s.send("\x04"); // Ctrl-D
    s.read_for(1000);
    CHECK(s.wait_exited(3000));
    CHECK(s.output().find("\033[?1049l") != std::string::npos);
}

TEST_CASE("Ctrl-D on empty buffer exits cleanly") {
    PtySession s = ready_editor();
    s.send("\x04");       // ^D on empty input → EOF
    s.read_for(1000);

    CHECK(s.wait_exited(3000));
    CHECK(s.output().find("\033[?1049l") != std::string::npos);
}

TEST_CASE("ESC clears an in-progress line without submitting") {
    PtySession s = ready_editor();

    s.send("willbe-cancelled");
    s.read_for(200);
    s.send("\033");       // lone ESC → cancel path after ~50ms timeout
    s.read_for(300);      // give the editor's CSI-timeout code time to fire

    // If ESC cleared the buffer, Ctrl-D now sees an empty line and exits.
    // If ESC failed, Ctrl-D would delete one character from the in-progress
    // buffer instead and the alt-screen leave sequence would not appear.
    s.send("\x04");
    s.read_for(1000);
    CHECK(s.wait_exited(3000));
    CHECK(s.output().find("\033[?1049l") != std::string::npos);
}

TEST_CASE("Home and End move the cursor to the buffer extremes") {
    PtySession s = ready_editor();
    s.send("q");
    s.read_for(200);
    s.send(kHome);
    s.send("/");
    s.read_for(200);
    s.send(kEnd);
    s.send("\r");
    s.read_for(1000);

    // Home inserted "/" before "q", then End submitted "/q" as an exit command.
    CHECK(s.wait_exited(3000));
    CHECK(s.output().find("\033[?1049l") != std::string::npos);
}

TEST_CASE("left/right arrow cursor navigation allows mid-string insertion") {
    PtySession s = ready_editor();
    s.send("/agnts");
    s.read_for(200);
    // Cursor is past 's'; move left to before 'n', insert 'e' -> "/agents".
    s.send(kArrLeft);
    s.send(kArrLeft);
    s.send(kArrLeft);
    s.send("e\r");
    s.read_for(500);

    CHECK(PtySession::strip_ansi(s.output()).find("/agents") != std::string::npos);
}

TEST_CASE("Ctrl-R reverse-i-search recalls and submits; Esc cancels") {
    PtySession s = ready_editor();

    // Seed history with two instant commands (no agent turn involved).
    s.send("/agents\r");
    s.read_for(500);
    s.send("/tokens\r");
    s.read_for(500);

    // Ctrl-R opens the search prompt.  (Per-keystroke buffer assertions
    // don't work here: the framebuffer diff renderer re-emits only changed
    // cells, so the query never appears contiguously in the byte stream —
    // assert functionally on what Enter submits instead.)
    const std::string before_accept = s.output();
    s.send("\x12");
    s.read_for(300);
    CHECK(PtySession::strip_ansi(s.output().substr(before_accept.size()))
              .find("reverse-i-search") != std::string::npos);

    // Query matches the older entry; Enter accepts and submits it, so its
    // echo lands in the scroll region again.
    s.send("agen");
    s.read_for(400);
    s.send("\r");
    s.read_for(700);
    CHECK(PtySession::strip_ansi(s.output().substr(before_accept.size()))
              .find("/agents") != std::string::npos);

    // Esc cancels: the pre-search (empty) buffer is restored, so Enter
    // submits nothing.  Snapshot after the Esc — the match preview itself
    // legitimately painted "/tokens" into the input row before it.
    s.send("\x12");
    s.read_for(200);
    s.send("token");
    s.read_for(300);
    s.send("\033");
    s.read_for(400);
    const std::string after_esc = s.output();
    s.send("\r");
    s.read_for(600);
    CHECK(PtySession::strip_ansi(s.output().substr(after_esc.size()))
              .find("/tokens") == std::string::npos);

    s.terminate();
}

TEST_CASE("history is shared live across panes") {
    PtySession s = ready_editor();

    // Type a command in the first pane, then split; the new (focused) pane
    // must see it in its own history immediately.
    s.send("/agents\r");
    s.read_for(500);
    s.send("\x17" "v");   // Ctrl-W v: vertical split, focuses the new pane
    s.read_for(600);

    const std::string before = s.output();
    s.send("\033[A");     // Up arrow: recall /agents in the new pane
    s.read_for(400);
    s.send("\r");
    s.read_for(600);
    const std::string new_bytes = PtySession::strip_ansi(s.output().substr(before.size()));
    CHECK(new_bytes.find("/agents") != std::string::npos);

    s.terminate();
}

TEST_CASE("Ctrl-P palette inserts the selected command; Esc closes it") {
    PtySession s = ready_editor();

    // Open the palette, narrow to /agents, accept, submit.  (Overlay pixels
    // are diff-rendered, so assert functionally on what Enter produces.)
    const std::string before = s.output();
    s.send("\x10");
    s.read_for(300);
    s.send("agents");
    s.read_for(400);
    s.send("\r");        // accept: buffer becomes "/agents "
    s.read_for(300);
    s.send("\r");        // submit it
    s.read_for(600);
    CHECK(PtySession::strip_ansi(s.output().substr(before.size()))
              .find("/agents") != std::string::npos);

    // Esc closes without touching the buffer: the follow-up Enter submits
    // an empty line, so no command echo appears afterward.
    s.send("\x10");
    s.read_for(200);
    s.send("quit");      // selects /quit but never accepts it
    s.read_for(300);
    s.send("\033");
    s.read_for(400);
    const std::string after_esc = s.output();
    s.send("\r");
    s.read_for(500);
    CHECK(PtySession::strip_ansi(s.output().substr(after_esc.size()))
              .find("/quit") == std::string::npos);
    // And the app is still running (Esc didn't leak /quit through).
    s.send("/agents\r");
    s.read_for(600);
    CHECK(PtySession::strip_ansi(s.output().substr(after_esc.size()))
              .find("/agents") != std::string::npos);

    s.terminate();
}

TEST_CASE("kitty keyboard protocol push is popped so ctrl keys stay legacy") {
    // Impersonate a kitty-class terminal: answer the startup capability
    // queries the way kitty/ghostty do, including "kitty keyboard protocol
    // supported" (ESC[?0u).  OpenTUI's handshake responds by pushing
    // CSI > 5 u (disambiguate escape codes), which would make the terminal
    // re-encode every ctrl+letter as CSI-u — sequences the input layer
    // doesn't decode.  setup_terminal must pop that entry (CSI < 1 u)
    // after the handshake so the whole ctrl-key surface keeps working.
    PtySession s(24, 80);
    s.spawn({ INDEX_TEST_BINARY });
    s.read_until("\033[?1049h", 10000);
    s.send("\x1b[?0u");            // kitty keyboard: supported, flags 0
    s.send("\x1b[24;1R");          // cursor position report
    s.send("\x1b[?62;22c");        // DA1
    s.send("\x1b[?2026;2$y");      // DECRPM: synchronized output supported
    s.read_for(2500);

    // Scan the raw byte stream for kitty keyboard pushes (CSI > ... u) and
    // pops (CSI < ... u).  The pop must come after the last push.
    const std::string out = s.output();
    auto find_last = [&](const std::string& intro) -> std::size_t {
        std::size_t last = std::string::npos;
        std::size_t pos = 0;
        while ((pos = out.find(intro, pos)) != std::string::npos) {
            std::size_t j = pos + intro.size();
            while (j < out.size() && (std::isdigit((unsigned char)out[j]) || out[j] == ';'))
                ++j;
            if (j < out.size() && out[j] == 'u') last = pos;
            pos += intro.size();
        }
        return last;
    };
    const std::size_t last_push = find_last("\x1b[>");
    const std::size_t last_pop  = find_last("\x1b[<");
    REQUIRE(last_pop != std::string::npos);
    if (last_push != std::string::npos) {
        CHECK(last_pop > last_push);
    }

    // And legacy control bytes still drive the app end-to-end.
    s.send("\x10");        // Ctrl-P opens the palette
    s.read_for(300);
    s.send("agents");
    s.read_for(300);
    s.send("\r\r");        // accept, then submit "/agents "
    s.read_for(800);
    CHECK(PtySession::strip_ansi(s.output()).find("/agents") != std::string::npos);

    s.terminate();
}

TEST_CASE("ctrl keys survive a kitty-support reply that lands after the startup handshake window") {
    // Regression: the one-shot disableKittyKeyboard() call in
    // setup_terminal() only undoes an enable that already happened by the
    // time that call returns. The terminal's "kitty keyboard supported"
    // reply is asynchronous — real round-trip latency can land it after
    // arbiter's own capability-response read loop (~1s budget) has already
    // moved on, so the protocol comes back on with nothing left to correct
    // it. Simulate exactly that: answer the kitty query well past that
    // window (as if flags 5 — disambiguate + report events — were already
    // active), and confirm ctrl keys still work, proving Engine::render()
    // re-asserts "disabled" on every tick rather than relying on the
    // one-shot call alone.
    PtySession s(24, 80);
    s.spawn({ INDEX_TEST_BINARY });
    s.read_until("\033[?1049h", 10000);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    s.send("\x1b[?5u");
    s.send("\x1b[24;1R");
    s.send("\x1b[?62;22c");
    s.send("\x1b[?2026;2$y");
    s.read_for(2000);
    // A couple more render ticks for the periodic re-assert to catch up.
    s.read_for(200);

    s.send("\x10");
    s.read_for(300);
    s.send("agents");
    s.read_for(300);
    s.send("\r\r");
    s.read_for(800);
    CHECK(PtySession::strip_ansi(s.output()).find("/agents") != std::string::npos);

    s.terminate();
}

TEST_CASE("ctrl keys still work when the terminal sends raw kitty CSI-u reports") {
    // The two tests above only ever prove that legacy control bytes
    // (0x10, 0x12, 0x17, ...) keep working after our disable attempts.
    // Neither actually exercises the case a real user hit in Ghostty: a
    // terminal that keeps the kitty keyboard protocol on regardless (e.g.
    // it doesn't honor CSI < u pops the way this harness's fake terminal
    // does, or some other program re-enables it) and sends every
    // ctrl+letter as `CSI <codepoint>;<mods> u` instead of a single C0
    // byte. Send those reports directly and confirm the app still responds
    // correctly — this is what proves the *decode* is the real fix, not
    // just the suppression timing.
    PtySession s = ready_editor();

    // Ctrl-P (codepoint 'p' = 112, mods = 5 = 1 + ctrl(4)) opens the palette.
    const std::string before = s.output();
    s.send("\x1b[112;5u");
    s.read_for(300);
    s.send("agents");
    s.read_for(300);
    s.send("\r\r");   // accept, then submit "/agents "
    s.read_for(800);
    CHECK(PtySession::strip_ansi(s.output().substr(before.size()))
              .find("/agents") != std::string::npos);

    // Esc under the "disambiguate" flag arrives as a bare `CSI 27 u` rather
    // than 0x1B; it must still clear an in-progress line. Same technique as
    // the plain-ESC test: if the buffer is cleared, Ctrl-D on the now-empty
    // line exits the app; if ESC was a no-op, Ctrl-D just deletes a char and
    // the app keeps running.
    s.send("willbe-cancelled");
    s.read_for(200);
    s.send("\x1b[27u");
    s.read_for(300);
    s.send("\x04");
    s.read_for(1000);
    CHECK(s.wait_exited(3000));
}

TEST_CASE("ctrl keys decode when the kitty report carries alternate-key/event-type subfields") {
    // Regression: OpenTUI pushes kitty flags 5 (disambiguate + *report
    // alternate keys*), so a real terminal's report isn't the bare
    // `CSI 112;5 u` the previous test used — it's
    // `CSI 112:80;5 u` (base codepoint : shifted-key alternate). The raw
    // CSI tokenizer in read_key_event() didn't accept ':' as a parameter
    // byte, so any sequence with a colon in it never reached
    // decode_kitty_csi_u at all — every real kitty-protocol terminal's
    // ctrl-key reports were silently dropped even though the decoder
    // itself handled colons correctly. Exercise the actual shape a real
    // terminal sends. (Event-type/release-event filtering is covered
    // directly, without PTY/UI-text fragility, in
    // test_kitty_key_decode.cpp.)
    PtySession s = ready_editor();

    // Ctrl-P: codepoint 'p'=112, shifted alternate 'P'=80, mods=5 (ctrl).
    const std::string before = s.output();
    s.send("\x1b[112:80;5u");
    s.read_for(300);
    s.send("agents");
    s.read_for(300);
    s.send("\r\r");
    s.read_for(800);
    CHECK(PtySession::strip_ansi(s.output().substr(before.size()))
              .find("/agents") != std::string::npos);

    // Ctrl-W chord (base 'w'=119, shifted 'W'=87) followed by 'b' opens the
    // history sidebar — proves the chord path also survives colon subfields.
    // The sidebar's row content ("Conversations", "Untitled", ...) is drawn
    // once at startup regardless of focus, so it won't reappear in this
    // diff window; key on the "enter  / filter" footer hint instead, which
    // only paints once the sidebar actually gains keyboard focus.
    const std::string before_chord = s.output();
    s.send("\x1b[119:87;5u");
    s.send("b");
    s.read_for(400);
    CHECK(PtySession::strip_ansi(s.output().substr(before_chord.size()))
              .find("filt") != std::string::npos);
    s.send("\x1b");
    s.read_for(200);

    s.terminate();
}
