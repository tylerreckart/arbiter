#pragma once
// index/include/tui/stream_filter.h — Tool-call line swallow for the streaming REPL
//
// The agent emits /fetch, /exec, /write … /endwrite, /agent, /mem, /advise
// lines directly in its streaming response.  When Config::verbose is false
// (the default), we don't want those lines to land in the scroll region —
// we want a single "N tool calls…" spinner in the status bar instead, and
// the model's synthesis prose around them rendered normally.
//
// StreamFilter is the thin line-buffered decorator that makes this happen.
// It wraps a downstream chunk callback (typically the one that feeds
// MarkdownRenderer), buffers bytes until it sees a newline, and then either
// passes the line through or swallows it based on what the line starts with.
//
// It does NOT count tool calls — the authoritative count comes from
// Orchestrator's post-exec ToolStatusFn, which fires once per real tool
// result with ok/fail status.  Detection here is strictly for *suppression*;
// counting here would double-count when the agent quotes a /cmd in a code
// fence or when the same /cmd fires through the dedup cache twice.

#include "config.h"

#include <functional>
#include <string>

namespace index_ai {

class StreamFilter {
public:
    using Sink = std::function<void(const std::string&)>;

    StreamFilter(const Config& cfg, Sink sink)
        : cfg_(cfg), sink_(std::move(sink)) {}

    // Feed a chunk of streaming bytes.  Complete lines are routed to the
    // sink (or swallowed); any partial trailing line is held in an internal
    // buffer until the next feed() / flush() completes it.
    void feed(const std::string& chunk);

    // Flush any buffered partial line to the sink.  Call once at end of
    // stream so the final line (if it lacks a trailing newline) still reaches
    // the renderer.  Safe to call multiple times.
    void flush();

    // Test hook — true iff we're currently swallowing lines because we're
    // inside an open /write … /endwrite block.
    bool in_write_block() const { return in_write_block_; }

private:
    // Decide whether a full line (without trailing newline) should be
    // swallowed.  Updates in_write_block_ as a side effect when it sees
    // /write or /endwrite.  Returns true if the line should be dropped.
    bool should_swallow(const std::string& line);

    const Config& cfg_;
    Sink          sink_;
    std::string   buf_;              // partial line carried across feeds
    bool          in_write_block_ = false;
};

} // namespace index_ai
