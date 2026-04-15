#pragma once
// PTY-based integration harness for TUI tests.
//
// Spawns a child process under a controlled pseudo-terminal so the TUI renders
// exactly as it would in a real terminal (alt-screen, DECSTBM scroll region,
// cursor save/restore, etc. all activate because isatty() returns true).
// Output is byte-accumulated; tests assert against the byte stream rather
// than a parsed screen — most TUI regressions are visible at the byte level.
//
// Isolation: every session gets a private temp HOME so the real ~/.index
// isn't touched, and ANTHROPIC_API_KEY=dummy so index refuses to make live
// API calls.  Session files land in the temp HOME and get blown away on
// destruct.
//
// NOT thread-safe; each test owns its own PtySession.

#include <chrono>
#include <string>
#include <vector>

namespace index_tests {

class PtySession {
public:
    // rows/cols match the terminal geometry negotiated via TIOCSWINSZ before
    // the child exec's.  The binary calls term_rows()/term_cols() on startup
    // so this controls the TUI layout.
    PtySession(int rows, int cols);
    ~PtySession();

    PtySession(const PtySession&) = delete;
    PtySession& operator=(const PtySession&) = delete;
    PtySession(PtySession&& o) noexcept;
    PtySession& operator=(PtySession&& o) noexcept;

    // Set an env var for the child.  Call before spawn().
    void env(const std::string& key, const std::string& value);

    // Fork+exec the given executable (absolute path) with argv.  The new
    // process sees the test env plus our isolation overrides (HOME, API key).
    void spawn(const std::vector<std::string>& argv);

    // Write raw bytes to the child's stdin.  Include "\r" for Enter.
    void send(const std::string& bytes);

    // Drain available bytes for `millis` milliseconds and return everything
    // seen so far.  Useful when asserting on an open-ended state like
    // "nothing more should appear".
    std::string read_for(int millis);

    // Read until `needle` appears in the accumulated output or `timeout_ms`
    // elapses.  Returns the full accumulated output (including bytes before
    // `needle`).  Throws if `needle` never appears — treat that as a test
    // failure rather than silently returning partial data.
    std::string read_until(const std::string& needle, int timeout_ms);

    // Cumulative output so far — everything the child has written to stdout
    // since spawn.  Tests can search this directly instead of re-reading.
    const std::string& output() const { return output_; }

    // Gracefully terminate the child (SIGTERM, wait up to 500ms, SIGKILL if
    // still alive).  Safe to call multiple times; destructor calls it too.
    void terminate();

    // Path to the isolated HOME directory so tests can inspect/write to it
    // (e.g. pre-populate ~/.index/agents before spawn).
    const std::string& home() const { return home_dir_; }

    // Return the accumulated output with ANSI CSI sequences (\033[…final)
    // stripped and alt-screen/cursor controls elided.  Useful when asserting
    // on user-visible text where the TUI's color codes would otherwise
    // split words (e.g. "> [dim]hi[rst]").  Doesn't attempt to rebuild a
    // coherent screen — the output still contains layout-relevant newlines.
    static std::string strip_ansi(const std::string& raw);

private:
    int  master_fd_ = -1;      // PTY master — we read/write this
    int  pid_       = -1;      // child pid, -1 if not spawned
    int  rows_, cols_;
    std::vector<std::pair<std::string, std::string>> env_;
    std::string output_;
    std::string home_dir_;     // temp HOME path

    // Low-level read with per-call timeout.  Appends to output_.  Returns
    // number of bytes read this call, or 0 on timeout / EOF.
    int drain_once(int timeout_ms);
};

} // namespace index_tests
