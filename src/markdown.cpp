// arbiter/src/markdown.cpp — Markdown renderer (styled spans + ANSI shim)

#include "markdown.h"
#include "render_policy.h"
#include "styled_text.h"
#include "theme.h"

#include <cctype>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

namespace arbiter {

namespace {

StyleId heading_style(size_t level) {
    switch (level) {
    case 1: return StyleId::Heading1;
    case 2: return StyleId::Heading2;
    case 3: return StyleId::Heading3;
    default: return StyleId::Heading4;
    }
}

bool starts_with_writ_name(std::string_view s, const char* name) {
    const size_t n = std::strlen(name);
    if (s.size() < n) return false;
    if (std::memcmp(s.data(), name, n) != 0) return false;
    if (s.size() == n) return true;
    const char next = s[n];
    return next == ' ' || next == '\t' || next == '\r';
}

bool is_markdown_writ_line(const std::string& line) {
    if (line.empty() || line[0] != '/') return false;
    static const char* kCmds[] = {
        "fetch", "exec", "agent", "pane", "write", "endwrite",
        "mem", "endmem", "read", "list", "browse", "search",
        "todo", "endtodo", "schedule", "mcp", "a2a", "parallel",
        "endparallel", "advise", "lesson", "endlesson", "help",
        nullptr
    };
    const std::string_view name(line.data() + 1, line.size() - 1);
    for (auto** p = kCmds; *p; ++p) {
        if (starts_with_writ_name(name, *p)) return true;
    }
    return false;
}

// UTF-8 "→ delegating:" status lines from the orchestrator gate.
bool is_delegation_status_line(const std::string& line, std::string_view& detail_out) {
    // "→ " is UTF-8 e2 86 92 20
    static constexpr char kArrow[] = "\xe2\x86\x92 ";
    static constexpr size_t kArrowLen = sizeof(kArrow) - 1;
    if (line.size() < kArrowLen) return false;
    if (std::memcmp(line.data(), kArrow, kArrowLen) != 0) return false;
    std::string_view rest(line.data() + kArrowLen, line.size() - kArrowLen);
    static constexpr char kDelegating[] = "delegating";
    static constexpr size_t kDelLen = sizeof(kDelegating) - 1;
    if (rest.size() < kDelLen) return false;
    for (size_t i = 0; i < kDelLen; ++i) {
        const char c = rest[i];
        const char lower = (c >= 'A' && c <= 'Z')
            ? static_cast<char>(c - 'A' + 'a') : c;
        if (lower != kDelegating[i]) return false;
    }
    rest.remove_prefix(kDelLen);
    if (!rest.empty() && rest.front() == ':') rest.remove_prefix(1);
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
        rest.remove_prefix(1);
    }
    detail_out = rest;
    return true;
}

void render_inline_styled(StyledLine& out, const std::string& text) {
    auto flush_plain = [&](size_t from, size_t to) {
        if (to > from) out.text.append(text, from, to - from);
    };

    size_t i = 0;
    size_t plain_start = 0;
    const size_t n = text.size();

    while (i < n) {
        bool matched = false;

        if (i + 2 < n && text[i] == '*' && text[i + 1] == '*' && text[i + 2] != '*') {
            size_t end = text.find("**", i + 2);
            if (end != std::string::npos && end > i + 2) {
                flush_plain(plain_start, i);
                styled_append(out, StyleId::Bold, text.substr(i + 2, end - i - 2));
                i = end + 2;
                plain_start = i;
                matched = true;
            }
        }
        if (!matched && text[i] == '*' && i + 1 < n && text[i + 1] != '*' && text[i + 1] != ' ') {
            size_t end = i + 1;
            bool found = false;
            while (end < n) {
                if (text[end] == '*' && text[end - 1] != ' ' &&
                    (end + 1 >= n || text[end + 1] != '*')) {
                    found = true;
                    break;
                }
                ++end;
            }
            if (found && end > i + 1) {
                flush_plain(plain_start, i);
                styled_append(out, StyleId::Italic, text.substr(i + 1, end - i - 1));
                i = end + 1;
                plain_start = i;
                matched = true;
            }
        }
        if (!matched && text[i] == '`') {
            if (i + 1 < n && text[i + 1] == '`') {
                size_t end = text.find("``", i + 2);
                if (end != std::string::npos) {
                    flush_plain(plain_start, i);
                    styled_append(out, StyleId::Code, text.substr(i + 2, end - i - 2));
                    i = end + 2;
                    plain_start = i;
                    matched = true;
                }
            } else {
                size_t end = text.find('`', i + 1);
                if (end != std::string::npos) {
                    flush_plain(plain_start, i);
                    styled_append(out, StyleId::Code, text.substr(i + 1, end - i - 1));
                    i = end + 1;
                    plain_start = i;
                    matched = true;
                }
            }
        }
        if (!matched && text[i] == '[') {
            size_t cb = text.find(']', i + 1);
            if (cb != std::string::npos && cb + 1 < n && text[cb + 1] == '(') {
                size_t cp = text.find(')', cb + 2);
                if (cp != std::string::npos) {
                    flush_plain(plain_start, i);
                    styled_append(out, StyleId::Link, text.substr(i + 1, cb - i - 1));
                    i = cp + 1;
                    plain_start = i;
                    matched = true;
                }
            }
        }
        if (!matched && i + 2 < n && text[i] == '~' && text[i + 1] == '~') {
            size_t end = text.find("~~", i + 2);
            if (end != std::string::npos) {
                flush_plain(plain_start, i);
                styled_append(out, StyleId::Strike, text.substr(i + 2, end - i - 2));
                i = end + 2;
                plain_start = i;
                matched = true;
            }
        }

        if (!matched) ++i;
    }
    flush_plain(plain_start, n);
}

bool is_hr(const std::string& line) {
    if (line.size() < 3) return false;
    char c = line[0];
    if (c != '-' && c != '*' && c != '_') return false;
    int count = 0;
    for (char ch : line) {
        if (ch == c) ++count;
        else if (ch != ' ') return false;
    }
    return count >= 3;
}

bool lang_eq_ci(std::string_view lang, std::string_view want) {
    if (lang.size() != want.size()) return false;
    for (size_t i = 0; i < lang.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(lang[i]);
        const unsigned char b = static_cast<unsigned char>(want[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

size_t fence_ltrim(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i;
}

bool is_fence_line(const std::string& line) {
    const size_t lead = fence_ltrim(line);
    const std::string_view view(line.data() + lead, line.size() - lead);
    return view.size() >= 3 &&
           (view.substr(0, 3) == "```" || view.substr(0, 3) == "~~~");
}

bool is_closing_fence_line(const std::string& line) {
    if (!is_fence_line(line)) return false;
    const size_t lead = fence_ltrim(line);
    const std::string_view view(line.data() + lead, line.size() - lead);
    return view.size() >= 3;
}

bool is_endwrite_line(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r'))
        --end;
    return end == 9 && s.compare(0, 9, "/endwrite") == 0;
}

} // namespace

bool MarkdownRenderer::is_diff_fence_lang(std::string_view lang) {
    lang = lang.substr(0, lang.find_first_of(" \t\r"));
    return lang_eq_ci(lang, "diff") || lang_eq_ci(lang, "patch");
}

void MarkdownRenderer::set_code_sink(CodeOpenFn on_open,
                                    CodeLineFn on_line,
                                    CodeCloseFn on_close) {
    code_open_ = std::move(on_open);
    code_line_ = std::move(on_line);
    code_close_ = std::move(on_close);
}

static std::string fence_lang(std::string_view tag) {
    tag = tag.substr(0, tag.find_first_of(" \t\r"));
    return std::string(tag);
}

void MarkdownRenderer::finish_code_close(const std::string& close_fence,
                                         std::vector<StyledLine>& out) {
    if (code_close_) {
        code_close_(close_fence);
    } else {
        auto block = render_buffered_code_block_styled(close_fence);
        for (auto& ln : block) out.push_back(std::move(ln));
    }
    code_buf_.clear();
    code_open_fence_.clear();
    in_code_block_ = false;
}

std::vector<StyledLine> MarkdownRenderer::render_buffered_code_block_styled(
    const std::string& close_fence) {
    std::vector<StyledLine> out;
    if (!code_open_fence_.empty()) {
        StyledLine fence;
        styled_append(fence, StyleId::Dim, code_open_fence_);
        out.push_back(std::move(fence));
    }
    for (const auto& body : code_buf_) {
        StyledLine line;
        styled_append(line, StyleId::Code, body);
        out.push_back(std::move(line));
    }
    if (!close_fence.empty()) {
        StyledLine fence;
        styled_append(fence, StyleId::Dim, close_fence);
        out.push_back(std::move(fence));
    }
    return out;
}

std::optional<StyledLine> MarkdownRenderer::process_line_styled(const std::string& line) {
    // Close indented-code blocks on the first non-empty, non-indented line.
    if (in_indent_code_) {
        const bool still_indent =
            line.empty() ||
            (line.size() >= 4 && line.compare(0, 4, "    ") == 0) ||
            (!line.empty() && line[0] == '\t');
        if (still_indent) {
            if (line.empty()) {
                if (code_line_) code_line_("");
            } else {
                size_t skip = (line[0] == '\t') ? 1 : 4;
                if (code_line_) code_line_(line.substr(skip));
            }
            return std::nullopt;
        }
        if (code_close_) code_close_("```");
        in_indent_code_ = false;
        // Fall through to render the terminating line as normal prose.
    }

    const size_t lead = fence_ltrim(line);
    const std::string_view view(line.data() + lead, line.size() - lead);

    if (view.size() >= 3 &&
        (view.substr(0, 3) == "```" || view.substr(0, 3) == "~~~")) {
        if (!in_code_block_) {
            in_code_block_ = true;
            in_diff_block_ = is_diff_fence_lang(view.substr(3));
            diff_buf_.clear();
            code_buf_.clear();
            code_open_fence_ = line;
            if (!in_diff_block_ && code_open_) {
                code_open_(line, fence_lang(view.substr(3)));
            }
            return std::nullopt;
        }
        if (in_diff_block_) {
            if (diff_sink_ && !diff_buf_.empty()) diff_sink_(diff_buf_);
            diff_buf_.clear();
            in_diff_block_ = false;
            in_code_block_ = false;
            code_open_fence_.clear();
            return std::nullopt;
        }
        // Non-diff closing fence: handled in feed_styled() so all body lines
        // emit together.  Return nullopt here to avoid duplicate output.
        return std::nullopt;
    }
    if (in_diff_block_) {
        if (!diff_buf_.empty()) diff_buf_ += '\n';
        diff_buf_ += line;
        return std::nullopt;
    }
    if (in_code_block_) {
        if (code_line_) {
            code_line_(line);
        } else {
            code_buf_.push_back(line);
        }
        return std::nullopt;
    }

    if (line.empty()) return StyledLine{};

    {
        std::string_view detail;
        if (is_delegation_status_line(line, detail)) {
            return styled_delegation_line(detail);
        }
    }

    if (is_markdown_writ_line(line)) {
        StyledLine cmd;
        // Distinctive glyph + bold writ color — stands apart from prose.
        styled_append(cmd, StyleId::WritLine, "\u203a ");  // ›
        styled_append(cmd, StyleId::WritLine, line);
        return cmd;
    }

    if (line[0] == '#') {
        size_t lvl = 0;
        while (lvl < line.size() && line[lvl] == '#') ++lvl;
        if (lvl < line.size() && line[lvl] == ' ') {
            StyledLine heading;
            std::string hashes(lvl, '#');
            styled_append(heading, heading_style(lvl), hashes + " ");
            render_inline_styled(heading, line.substr(lvl + 1));
            return heading;
        }
    }

    if (is_hr(line)) {
        StyledLine rule;
        styled_append(rule, StyleId::Rule, std::string(60, '-'));
        return rule;
    }

    if (line[0] == '>' && (line.size() == 1 || line[1] == ' ')) {
        StyledLine quote;
        std::string content = line.size() > 2 ? line.substr(2) : "";
        styled_append(quote, StyleId::Blockquote, "\u2502 ");
        render_inline_styled(quote, content);
        return quote;
    }

    {
        size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') ++indent;
        if (indent < line.size() &&
            (line[indent] == '-' || line[indent] == '*' || line[indent] == '+') &&
            indent + 1 < line.size() && line[indent + 1] == ' ') {
            // Task lists: `- [ ] …` / `- [x] …` (GFM-style).
            const size_t after_bullet = indent + 2;
            bool is_task = false;
            bool task_done = false;
            if (after_bullet + 3 <= line.size() &&
                line[after_bullet] == '[' &&
                (line[after_bullet + 1] == ' ' ||
                 line[after_bullet + 1] == 'x' ||
                 line[after_bullet + 1] == 'X') &&
                line[after_bullet + 2] == ']' &&
                (after_bullet + 3 == line.size() || line[after_bullet + 3] == ' ')) {
                is_task = true;
                task_done = (line[after_bullet + 1] != ' ');
            }

            StyledLine bullet_line;
            if (indent > 0) {
                styled_append(bullet_line, StyleId::Default, std::string(indent, ' '));
            }
            if (is_task) {
                styled_append(bullet_line,
                              task_done ? StyleId::Success : StyleId::Dim,
                              task_done ? "\u2611 " : "\u2610 ");  // ☑ / ☐
                const size_t text_at = (after_bullet + 3 < line.size() &&
                                        line[after_bullet + 3] == ' ')
                    ? after_bullet + 4
                    : after_bullet + 3;
                render_inline_styled(bullet_line, line.substr(text_at));
            } else {
                const char* bullet = (indent == 0) ? "\xe2\x80\xa2"
                                   : (indent <= 2)  ? "\xe2\x97\xa6"
                                                    : "\xe2\x80\x93";
                styled_append(bullet_line, StyleId::Bullet, bullet);
                styled_append(bullet_line, StyleId::Default, " ");
                render_inline_styled(bullet_line, line.substr(indent + 2));
            }
            return bullet_line;
        }
    }

    // Indented code → CodeSegment when a code sink is wired; otherwise prose.
    if ((line.size() >= 4 && line.compare(0, 4, "    ") == 0) ||
        (!line.empty() && line[0] == '\t')) {
        size_t skip = (line[0] == '\t') ? 1 : 4;
        const std::string body = line.substr(skip);
        if (code_open_ && code_line_) {
            if (!in_indent_code_) {
                in_indent_code_ = true;
                code_open_("```", "");
            }
            code_line_(body);
            return std::nullopt;
        }
        StyledLine code_line;
        styled_append(code_line, StyleId::Default, "    ");
        styled_append(code_line, StyleId::Code, body);
        return code_line;
    }

    // Nested numbered lists: allow leading indent before `1. `.
    {
        size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') ++indent;
        size_t dig = indent;
        while (dig < line.size() && std::isdigit(static_cast<unsigned char>(line[dig])))
            ++dig;
        if (dig > indent && dig < line.size() && line[dig] == '.' &&
            dig + 1 < line.size() && line[dig + 1] == ' ') {
            StyledLine numbered;
            if (indent > 0) {
                styled_append(numbered, StyleId::Default, std::string(indent, ' '));
            }
            styled_append(numbered, StyleId::Bold, line.substr(indent, dig - indent + 1));
            styled_append(numbered, StyleId::Default, " ");
            render_inline_styled(numbered, line.substr(dig + 2));
            return numbered;
        }
    }

    StyledLine plain;
    render_inline_styled(plain, line);
    return plain;
}

std::vector<StyledLine> MarkdownRenderer::feed_styled(const std::string& chunk) {
    std::vector<StyledLine> result;
    for (char c : chunk) {
        if (c == '\n') {
            if (!seen_content_ && line_buf_.empty()) continue;
            if (is_endwrite_line(line_buf_)) {
                line_buf_.clear();
                continue;
            }
            seen_content_ = true;
            const bool was_in_diff = in_diff_block_;

            if (in_code_block_ && !in_diff_block_ && is_closing_fence_line(line_buf_)) {
                finish_code_close(line_buf_, result);
            } else if (auto line_out = process_line_styled(line_buf_)) {
                result.push_back(std::move(*line_out));
            } else if (!was_in_diff && !in_code_block_ && line_buf_.empty()) {
                result.push_back(StyledLine{});
            }
            line_buf_.clear();
        } else if (c != '\r') {
            line_buf_ += c;
        }
    }
    return result;
}

std::string MarkdownRenderer::feed(const std::string& chunk) {
    const auto lines = feed_styled(chunk);
    std::string result;
    for (const StyledLine& line : lines) {
        const std::string rendered = to_ansi(line);
        if (rendered.empty()) {
            result += '\n';
        } else {
            result += rendered;
            if (result.back() != '\n') result += '\n';
        }
    }
    return result;
}

std::vector<StyledLine> MarkdownRenderer::flush_styled() {
    std::vector<StyledLine> result;
    if (!line_buf_.empty()) {
        if (is_endwrite_line(line_buf_)) {
            line_buf_.clear();
            return result;
        }
        if (auto line_out = process_line_styled(line_buf_)) {
            result.push_back(std::move(*line_out));
        }
        line_buf_.clear();
        return result;
    }
    if (in_diff_block_ && diff_sink_ && !diff_buf_.empty()) {
        diff_sink_(diff_buf_);
        diff_buf_.clear();
        in_diff_block_ = false;
        in_code_block_ = false;
        code_open_fence_.clear();
    } else if (in_code_block_ && (!code_buf_.empty() || code_open_)) {
        if (code_close_) {
            code_close_("");
        } else if (!code_buf_.empty()) {
            auto block = render_buffered_code_block_styled("");
            for (auto& ln : block) result.push_back(std::move(ln));
        }
        code_buf_.clear();
        code_open_fence_.clear();
        in_code_block_ = false;
    }
    if (in_indent_code_) {
        if (code_close_) code_close_("```");
        in_indent_code_ = false;
    }
    return result;
}

std::string MarkdownRenderer::flush() {
    return styled_lines_to_ansi(flush_styled());
}

void MarkdownRenderer::reset() {
    line_buf_.clear();
    in_code_block_ = false;
    in_diff_block_ = false;
    in_indent_code_ = false;
    diff_buf_.clear();
    code_buf_.clear();
    code_open_fence_.clear();
    code_open_ = nullptr;
    code_line_ = nullptr;
    code_close_ = nullptr;
    seen_content_ = false;
}

std::string render_markdown(const std::string& text) {
    MarkdownRenderer r;
    std::string result = r.feed(text);
    std::string tail = r.flush();
    if (!tail.empty()) result += tail;
    return result;
}

std::string render_diff_ansi(const std::string& patch) {
    const Theme& t = theme();
    std::string out;
    size_t start = 0;
    while (start <= patch.size()) {
        size_t end = patch.find('\n', start);
        std::string_view line(patch.data() + start,
                              (end == std::string::npos) ? patch.size() - start
                                                         : end - start);
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (line.size() >= 3 && line.substr(0, 3) == "+++") {
            out += t.accent_info;
            out += line;
            out += t.reset;
        } else if (line.size() >= 3 && line.substr(0, 3) == "---") {
            out += t.accent_info;
            out += line;
            out += t.reset;
        } else if (line.size() >= 2 && line.substr(0, 2) == "@@") {
            out += t.dim;
            out += line;
            out += t.reset;
        } else if (!line.empty() && line[0] == '+') {
            out += t.accent_success;
            out += line;
            out += t.reset;
        } else if (!line.empty() && line[0] == '-') {
            out += t.accent_error;
            out += line;
            out += t.reset;
        } else {
            out += t.md_code;
            out += line;
            out += t.reset;
        }
        out += '\n';

        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

} // namespace arbiter
