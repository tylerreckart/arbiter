#include "tui/block_parser.h"

#include <cstring>
#include <string_view>

namespace arbiter {

namespace {

bool starts_with_cmd(std::string_view s, const char* prefix, size_t plen) {
    if (s.size() < plen) return false;
    if (std::memcmp(s.data(), prefix, plen) != 0) return false;
    if (s.size() == plen) return true;
    const char next = s[plen];
    return next == ' ' || next == '\t' || next == '\r';
}

bool is_framing_marker(std::string_view s) {
    if (s.empty() || s[0] != '[') return false;
    static const char* kOpens[] = {
        "[/fetch", "[/exec", "[/write", "[/agent", "[/mem", "[/advise",
        "[/read", "[/browse", "[/search", "[/todo", "[/schedule", "[/mcp",
        "[/a2a", "[/list", "[/map", "[/parallel", "[/pane", "[/lesson",
        "[END ", "[TOOL RESULTS", "[END TOOL RESULTS"
    };
    for (const char* p : kOpens) {
        const size_t n = std::strlen(p);
        if (s.size() >= n && std::memcmp(s.data(), p, n) == 0) return true;
    }
    return false;
}

size_t ltrim_idx(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return i;
}

// Agent writ prefixes swallowed in quiet mode (and styled when verbose).
bool is_agent_writ_line(std::string_view s) {
    if (s.empty() || s[0] != '/') return false;
    static const char* kCmds[] = {
        "fetch", "exec", "agent", "pane", "write", "endwrite",
        "mem", "endmem", "read", "list", "map", "browse", "search",
        "todo", "endtodo", "schedule", "mcp", "a2a", "parallel",
        "endparallel", "advise", "lesson", "endlesson", "help",
        nullptr
    };
    const std::string_view name = s.substr(1);
    for (auto** p = kCmds; *p; ++p) {
        if (starts_with_cmd(name, *p, std::strlen(*p))) return true;
    }
    return false;
}

} // namespace

BlockParser::BlockParser(bool show_writs, Sink sink)
    : show_writs_(show_writs), sink_(std::move(sink)) {}

bool BlockParser::should_swallow(const std::string& line) {
    if (show_writs_) return false;

    std::string trimmed = line;
    if (!trimmed.empty() && trimmed.back() == '\r') trimmed.pop_back();
    const size_t lead = ltrim_idx(trimmed);
    std::string_view view(trimmed);
    view.remove_prefix(lead);

    if (in_write_block_) {
        if (view.size() >= 9 &&
            std::memcmp(view.data(), "/endwrite", 9) == 0 &&
            (view.size() == 9 || view[9] == ' ' || view[9] == '\t' || view[9] == '\r')) {
            in_write_block_ = false;
        }
        return true;
    }

    if (in_todo_block_) {
        if (view.size() >= 8 &&
            std::memcmp(view.data(), "/endtodo", 8) == 0 &&
            (view.size() == 8 || view[8] == ' ' || view[8] == '\t' || view[8] == '\r')) {
            in_todo_block_ = false;
        }
        return true;
    }

    // After `/todo add …`, the next non-writ line begins a block body.
    if (pending_todo_body_) {
        pending_todo_body_ = false;
        if (!view.empty() && !is_agent_writ_line(view)) {
            in_todo_block_ = true;
            return true;
        }
        // Empty line or another writ — single-line /todo add; fall through.
    }

    if (view.size() > 7 && std::memcmp(view.data(), "/write ", 7) == 0) {
        in_write_block_ = true;
        return true;
    }

    if (view.size() >= 9 && std::memcmp(view.data(), "/todo add", 9) == 0 &&
        (view.size() == 9 || view[9] == ' ' || view[9] == '\t')) {
        pending_todo_body_ = true;
        return true;
    }

    if (is_agent_writ_line(view)) return true;
    if (is_framing_marker(view)) return true;

    return false;
}

namespace {

bool is_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

int utf8_code_unit_count(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

void peel_incomplete_utf8(std::string& text, std::string& hold) {
    if (text.empty()) return;
    size_t i = text.size();
    while (i > 0) {
        --i;
        if (!is_continuation(static_cast<unsigned char>(text[i]))) break;
    }

    const unsigned char lead = static_cast<unsigned char>(text[i]);
    const int need = utf8_code_unit_count(lead);
    if (need <= 1) return;

    const size_t have = text.size() - i;
    if (have >= static_cast<size_t>(need)) return;

    hold.append(text.begin() + static_cast<std::ptrdiff_t>(i), text.end());
    text.resize(i);
}

} // namespace

bool BlockParser::can_emit_partial(const std::string& buf) const {
    if (in_write_block_ || in_todo_block_ || pending_todo_body_) return false;
    const size_t lead = ltrim_idx(buf);
    if (lead == buf.size()) return false;  // all whitespace — may precede a /cmd
    const char c = buf[lead];
    return c != '/' && c != '[';
}

void BlockParser::feed(std::string_view chunk) {
    if (show_writs_) {
        if (!chunk.empty()) sink_(chunk);
        return;
    }

    buf_.append(chunk.data(), chunk.size());

    std::string passthrough;
    size_t start = 0;
    for (size_t i = 0; i < buf_.size(); ++i) {
        if (buf_[i] != '\n') continue;
        const std::string line = buf_.substr(start, i - start);
        if (!should_swallow(line)) {
            passthrough.append(buf_, start, i - start + 1);
        }
        start = i + 1;
    }

    if (start > 0) buf_.erase(0, start);
    if (!passthrough.empty()) sink_(passthrough);

    // Unambiguous incomplete prose — emit now so the live tail can paint.
    if (!buf_.empty() && can_emit_partial(buf_)) {
        std::string emit = utf8_hold_;
        utf8_hold_.clear();
        emit += buf_;
        peel_incomplete_utf8(emit, utf8_hold_);
        if (!emit.empty()) sink_(emit);
        buf_.clear();
    }
}

void BlockParser::flush() {
    if (buf_.empty() && utf8_hold_.empty()) return;
    std::string tail = utf8_hold_;
    utf8_hold_.clear();
    tail += buf_;
    buf_.clear();
    if (show_writs_ || !should_swallow(tail)) {
        sink_(tail);
    }
}

void BlockParser::reset() {
    buf_.clear();
    utf8_hold_.clear();
    in_write_block_ = false;
    in_todo_block_ = false;
    pending_todo_body_ = false;
}

} // namespace arbiter
