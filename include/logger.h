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

} // namespace arbiter
