// index_ai/src/markdown.cpp — Markdown-to-ANSI terminal renderer

#include "markdown.h"
#include <cctype>
#include <string>

namespace index_ai {

// ─── ANSI primitives ─────────────────────────────────────────────────────────

static const char* RST  = "\033[0m";
static const char* BOLD = "\033[1m";
static const char* DIM  = "\033[2m";
static const char* ITAL = "\033[3m";
static const char* UNDL = "\033[4m";
static const char* STRK = "\033[9m";

static std::string fg(int n) {
    return "\033[38;5;" + std::to_string(n) + "m";
}
static std::string bold_fg(int n) {
    return "\033[1;38;5;" + std::to_string(n) + "m";
}

// ─── Inline renderer ─────────────────────────────────────────────────────────

static std::string render_inline(const std::string& text) {
    std::string result;
    size_t i = 0;
    const size_t n = text.size();

    while (i < n) {
        // Bold: **text**
        if (i + 2 < n && text[i] == '*' && text[i+1] == '*' && text[i+2] != '*') {
            size_t end = text.find("**", i + 2);
            if (end != std::string::npos && end > i + 2) {
                result += BOLD;
                result += text.substr(i + 2, end - i - 2);
                result += RST;
                i = end + 2;
                continue;
            }
        }
        // Italic: *text* (single star, not adjacent to another *)
        if (text[i] == '*' && i + 1 < n && text[i+1] != '*' && text[i+1] != ' ') {
            size_t end = i + 1;
            bool found = false;
            while (end < n) {
                if (text[end] == '*' && text[end-1] != ' ' &&
                    (end + 1 >= n || text[end+1] != '*')) {
                    found = true;
                    break;
                }
                ++end;
            }
            if (found && end > i + 1) {
                result += ITAL;
                result += text.substr(i + 1, end - i - 1);
                result += RST;
                i = end + 1;
                continue;
            }
        }
        // Inline code: `text` or ``text``
        if (text[i] == '`') {
            if (i + 1 < n && text[i+1] == '`') {
                // Double backtick
                size_t end = text.find("``", i + 2);
                if (end != std::string::npos) {
                    result += fg(214);
                    result += text.substr(i + 2, end - i - 2);
                    result += RST;
                    i = end + 2;
                    continue;
                }
            } else {
                size_t end = text.find('`', i + 1);
                if (end != std::string::npos) {
                    result += fg(214);  // amber
                    result += text.substr(i + 1, end - i - 1);
                    result += RST;
                    i = end + 1;
                    continue;
                }
            }
        }
        // Link: [text](url)
        if (text[i] == '[') {
            size_t cb = text.find(']', i + 1);
            if (cb != std::string::npos && cb + 1 < n && text[cb+1] == '(') {
                size_t cp = text.find(')', cb + 2);
                if (cp != std::string::npos) {
                    result += std::string(UNDL) + fg(208);  // underline + bright orange
                    result += text.substr(i + 1, cb - i - 1);
                    result += RST;
                    i = cp + 1;
                    continue;
                }
            }
        }
        // Strikethrough: ~~text~~
        if (i + 2 < n && text[i] == '~' && text[i+1] == '~') {
            size_t end = text.find("~~", i + 2);
            if (end != std::string::npos) {
                result += STRK;
                result += text.substr(i + 2, end - i - 2);
                result += RST;
                i = end + 2;
                continue;
            }
        }
        result += text[i++];
    }
    return result;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

static bool is_hr(const std::string& line) {
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

// ─── Line renderer ───────────────────────────────────────────────────────────

std::string MarkdownRenderer::process_line(const std::string& line) {
    // Fenced code block toggle (``` or ~~~)
    if (line.size() >= 3 &&
        (line.substr(0, 3) == "```" || line.substr(0, 3) == "~~~")) {
        in_code_block_ = !in_code_block_;
        return std::string(DIM) + line + RST;
    }
    // Code block body: amber
    if (in_code_block_) {
        return fg(214) + line + RST;
    }

    // Empty line
    if (line.empty()) return line;

    // Agent-issued command lines (/fetch, /exec, /agent, /write, /mem, /endwrite)
    if (!line.empty() && line[0] == '/') {
        static const char* kCmdPrefixes[] = {
            "/fetch ", "/exec ", "/agent ", "/write ", "/mem ", "/endwrite", nullptr
        };
        for (auto** p = kCmdPrefixes; *p; ++p) {
            size_t plen = strlen(*p);
            if (line.size() >= plen && line.compare(0, plen, *p) == 0) {
                return fg(214) + std::string(DIM) + "> " + line + fg(252) + RST + "\n";
            }
        }
    }

    // Headings: # ## ### ####
    if (line[0] == '#') {
        size_t lvl = 0;
        while (lvl < line.size() && line[lvl] == '#') ++lvl;
        if (lvl < line.size() && line[lvl] == ' ') {
            // 256-color palette: bright orange, amber, medium orange, burnt orange
            static const int colors[] = {208, 214, 172, 166};
            int color = colors[std::min(lvl - 1, size_t(3))];
            std::string hashes(lvl, '#');
            std::string content = render_inline(line.substr(lvl + 1));
            return bold_fg(color) + hashes + " " + content + RST;
        }
    }

    // Horizontal rule
    if (is_hr(line)) {
        return std::string(DIM) + std::string(60, '-') + RST;
    }

    // Blockquote: > text
    if (line[0] == '>' && (line.size() == 1 || line[1] == ' ')) {
        std::string content = line.size() > 2 ? line.substr(2) : "";
        return std::string(DIM) + "\u2502 " + render_inline(content) + RST;
    }

    // Bullet list item (-, *, +), possibly indented
    {
        size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') ++indent;
        if (indent < line.size() &&
            (line[indent] == '-' || line[indent] == '*' || line[indent] == '+') &&
            indent + 1 < line.size() && line[indent+1] == ' ') {
            std::string pad(indent, ' ');
            // Alternate bullet symbol by indent level
            const char* bullet = (indent == 0) ? "\xe2\x80\xa2"   // •
                               : (indent <= 2)  ? "\xe2\x97\xa6"   // ◦
                                                : "\xe2\x80\x93";  // –
            std::string content = render_inline(line.substr(indent + 2));
            return pad + fg(252) + bullet + RST + " " + content;
        }
    }

    // Indented code block (4 spaces or tab)
    if ((line.size() >= 4 && line.substr(0, 4) == "    ") ||
        (!line.empty() && line[0] == '\t')) {
        size_t skip = (line[0] == '\t') ? 1 : 4;
        return std::string("    ") + fg(214) + line.substr(skip) + RST;
    }

    // Numbered list: 1. 2. 10. etc.
    if (!line.empty() && std::isdigit(static_cast<unsigned char>(line[0]))) {
        size_t dot = 0;
        while (dot < line.size() && std::isdigit(static_cast<unsigned char>(line[dot]))) ++dot;
        if (dot < line.size() && line[dot] == '.' &&
            dot + 1 < line.size() && line[dot+1] == ' ') {
            std::string num = line.substr(0, dot + 1);
            std::string content = render_inline(line.substr(dot + 2));
            return std::string(BOLD) + num + RST + " " + content;
        }
    }

    // Plain text: apply inline styling only
    return render_inline(line);
}

// ─── MarkdownRenderer methods ─────────────────────────────────────────────────

// True if `s`, after trimming trailing whitespace, is exactly `/endwrite`.
// The /endwrite sentinel closes a /write block but has no value to the human
// reading the session — the file content itself is the informative part, and
// the write result is reported via the subsequent [/write ...] tool block.
static bool is_endwrite_line(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end-1] == ' ' || s[end-1] == '\t' || s[end-1] == '\r'))
        --end;
    return end == 9 && s.compare(0, 9, "/endwrite") == 0;
}

std::string MarkdownRenderer::feed(const std::string& chunk) {
    std::string result;
    for (char c : chunk) {
        if (c == '\n') {
            // Drop leading blank lines — the REPL already padded below the
            // user prompt, so echoing another blank would stack them.
            if (!seen_content_ && line_buf_.empty()) continue;
            // Hide the /endwrite sentinel from the session window.  It's a
            // parser-facing marker, not content the user needs to see.
            if (is_endwrite_line(line_buf_)) {
                line_buf_.clear();
                continue;
            }
            seen_content_ = true;
            result += process_line(line_buf_);
            result += '\n';
            line_buf_.clear();
        } else if (c != '\r') {
            line_buf_ += c;
        }
    }
    return result;
}

std::string MarkdownRenderer::flush() {
    if (line_buf_.empty()) return {};
    if (is_endwrite_line(line_buf_)) { line_buf_.clear(); return {}; }
    std::string result = process_line(line_buf_);
    line_buf_.clear();
    // Always terminate with a newline so the caller can append a single
    // separator `\n` and get exactly one blank line — regardless of whether
    // the model's last chunk had a trailing newline or not.
    if (result.empty() || result.back() != '\n') result += '\n';
    return result;
}

void MarkdownRenderer::reset() {
    line_buf_.clear();
    in_code_block_ = false;
    seen_content_  = false;
}

// ─── Convenience: full-document render ───────────────────────────────────────

std::string render_markdown(const std::string& text) {
    MarkdownRenderer r;
    std::string result = r.feed(text);
    std::string tail   = r.flush();
    if (!tail.empty()) result += tail;
    return result;
}

} // namespace index_ai
