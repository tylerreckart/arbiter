// index/src/tui/stream_filter.cpp
#include "tui/stream_filter.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace index_ai {

namespace {

// True iff `s` starts with `prefix` followed by whitespace or end-of-string.
// Agent tool calls look like "/exec ls -la" or "/endwrite" with no args, so
// we accept both shapes.  The (char)-after check prevents false positives
// against things like "/execute" (not a real command, but a cheap guard).
bool starts_with_cmd(const std::string& s, const char* prefix, size_t plen) {
    if (s.size() < plen) return false;
    if (std::memcmp(s.data(), prefix, plen) != 0) return false;
    if (s.size() == plen) return true;
    char next = s[plen];
    return next == ' ' || next == '\t' || next == '\r';
}

// True iff the line is a tool-result framing marker emitted by
// execute_agent_commands ([TOOL RESULTS], [END EXEC], [/exec …], etc.).
// These never reach the user stream today because execute_agent_commands'
// output is fed back into the agent rather than to `cb`, but the agent
// frequently quotes them verbatim in its synthesis.  Swallow them so the
// user doesn't see meta-framing they can't act on.
bool is_framing_marker(const std::string& s) {
    if (s.empty() || s[0] != '[') return false;
    // "[/fetch …]", "[/exec …]", "[/write …]", "[/agent …]", "[/mem …]",
    // "[/advise]", "[END …]", "[TOOL RESULTS …]"
    static const char* kOpens[] = {
        "[/fetch", "[/exec", "[/write", "[/agent", "[/mem", "[/advise",
        "[END ", "[TOOL RESULTS", "[END TOOL RESULTS"
    };
    for (const char* p : kOpens) {
        size_t n = std::strlen(p);
        if (s.size() >= n && std::memcmp(s.data(), p, n) == 0) return true;
    }
    return false;
}

// Trim a leading run of spaces/tabs for prefix checks without allocating.
// Returns the index of the first non-whitespace character.
size_t ltrim_idx(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return i;
}

} // anonymous namespace

bool StreamFilter::should_swallow(const std::string& line) {
    // Strip CR and leading whitespace for the decision, but the line passed
    // to the sink is unchanged — we don't reformat, only route.
    std::string trimmed = line;
    if (!trimmed.empty() && trimmed.back() == '\r') trimmed.pop_back();
    size_t lead = ltrim_idx(trimmed);
    std::string_view view(trimmed);
    view.remove_prefix(lead);

    // While inside a /write block, everything between /write and /endwrite
    // is file content — swallow it all, then exit the block when we see
    // /endwrite.  /endwrite itself is also swallowed.
    if (in_write_block_) {
        if (view.size() >= 9 &&
            std::memcmp(view.data(), "/endwrite", 9) == 0 &&
            (view.size() == 9 || view[9] == ' ' || view[9] == '\t' || view[9] == '\r')) {
            in_write_block_ = false;
        }
        return true;
    }

    // /write <path> opens a new block.  Swallow this line and enter the
    // multi-line swallow state until /endwrite.
    if (view.size() > 7 &&
        std::memcmp(view.data(), "/write ", 7) == 0) {
        in_write_block_ = true;
        return true;
    }

    // Single-line tool-call commands.
    std::string s(view);
    if (starts_with_cmd(s, "/fetch", 6))  return true;
    if (starts_with_cmd(s, "/exec", 5))   return true;
    if (starts_with_cmd(s, "/agent", 6))  return true;
    if (starts_with_cmd(s, "/mem", 4))    return true;
    if (starts_with_cmd(s, "/advise", 7)) return true;

    // Tool-result framing markers the agent may echo in prose.
    if (is_framing_marker(s)) return true;

    return false;
}

void StreamFilter::feed(const std::string& chunk) {
    // Verbose: pure pass-through.  No buffering — the caller's markdown
    // renderer already handles its own partial-line state.
    if (cfg_.verbose) {
        sink_(chunk);
        return;
    }

    // Non-verbose: line-split, filter, flush passed lines to the sink as
    // coalesced strings (so the markdown renderer keeps seeing normal
    // multi-line chunks and its heading/code-fence state stays consistent).
    buf_ += chunk;

    std::string passthrough;
    size_t start = 0;
    for (size_t i = 0; i < buf_.size(); ++i) {
        if (buf_[i] != '\n') continue;
        std::string line = buf_.substr(start, i - start);
        if (!should_swallow(line)) {
            passthrough.append(buf_, start, i - start + 1);  // include '\n'
        }
        start = i + 1;
    }

    if (start > 0) {
        buf_.erase(0, start);
    }
    if (!passthrough.empty()) sink_(passthrough);
}

void StreamFilter::flush() {
    if (buf_.empty()) return;
    // Treat the partial tail as a line for swallow purposes.  If it shouldn't
    // be swallowed, send it (without a synthetic newline — MarkdownRenderer
    // handles its own tail).
    if (cfg_.verbose || !should_swallow(buf_)) {
        sink_(buf_);
    }
    buf_.clear();
}

} // namespace index_ai
