#pragma once
// arbiter/include/logger.h
//
// Process-wide structured logger.  Operator-controlled output format
// (human / JSON) via ARBITER_LOG_FORMAT.  Replaces the bare
// `fprintf(stderr, ...)` calls in the API server's operational paths
// (startup, recovery sweep, drain, sandbox lifecycle) so a deployment
// running under journald / Loki / ELK can ingest events as structured
// records rather than parsing free-form text.
//
// This is NOT a full event-mirror replacement: the SSE per-event log
// stream (--verbose) keeps its existing renderer for the human case.
// JSON mode is the structured wire format intended for log aggregators.
//
// Scope of v1:
//   * Process-singleton, thread-safe.
//   * Three levels: info / warn / error.
//   * Key=value field pairs, all values string-typed (no nested
//     objects, no numeric typing).  Aggregators that need types
//     parse the value strings themselves; the wire stays simple.

#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace arbiter {

enum class LogFormat { Human, Json };

class Logger {
public:
    using Field  = std::pair<std::string, std::string>;
    using Fields = std::initializer_list<Field>;

    static Logger& global();

    // Read the configured format from env (ARBITER_LOG_FORMAT) once.
    // Idempotent; subsequent calls have no effect.  CLI binaries call
    // this from main() before spinning the server.
    void init_from_env();

    void set_format(LogFormat fmt);
    LogFormat format() const;

    void info (const std::string& event, Fields fields = {});
    void warn (const std::string& event, Fields fields = {});
    void error(const std::string& event, Fields fields = {});

private:
    Logger() = default;
    void log_impl(const char* level, const std::string& event,
                   const std::vector<Field>& fields);

    mutable std::mutex mu_;
    LogFormat          fmt_ = LogFormat::Human;
    bool               initialized_ = false;
};

// While the interactive TUI owns the terminal, stderr is appended to
// `<config_dir>/tui.log` instead of the tty. stdout stays the renderer
// (OpenTUI frames). Every direct stderr writer during a session —
// Logger, the openrouter_response fprintf, intent_route / intent_shadow,
// scheduler, compaction, constitution warnings, the OpenTUI log bridge —
// follows this fd. Restores the previous stderr on destruction.
//
// Audited stdout during TUI mode is the frame itself, a flush before the
// input loop, and a single newline after alt-screen teardown. Operational
// logs are stderr. `print_turn_rule` writes stdout but has no callers.
class TuiStderrRedirect {
public:
    explicit TuiStderrRedirect(const std::string& config_dir);
    ~TuiStderrRedirect();

    TuiStderrRedirect(const TuiStderrRedirect&) = delete;
    TuiStderrRedirect& operator=(const TuiStderrRedirect&) = delete;

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] const std::string& path() const { return path_; }

private:
    std::string path_;
    int saved_fd_ = -1;
    bool active_ = false;
};

// ARBITER_TUI_LOG_PROBE=1 emits openrouter_response, intent_route, and
// intent_shadow on stderr. Used by the PTY regression test to fire those
// lines from a turn thread. No-op otherwise.
void emit_tui_log_probe_if_enabled();

} // namespace arbiter
