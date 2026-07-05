// arbiter/src/markdown.cpp — Markdown-to-ANSI terminal renderer

#include "markdown.h"
#include "theme.h"
#include <cctype>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

namespace arbiter {

namespace {

constexpr size_t kCodeBlockPreviewLines = 8;

} // anonymous namespace

// ─── ANSI primitives ─────────────────────────────────────────────────────────
// Attribute-only escapes stay hard-coded — they're theme-agnostic (dim /
// bold / italic / underline / strikethrough are character attributes, not
// colors).  All foreground colors are pulled from the global Theme so
// swapping theme recolors markdown rendering with no edits here.

static const char* RST  = "\033[0m";
static const char* BOLD = "\033[1m";
static const char* DIM  = "\033[2m";
static const char* ITAL = "\033[3m";
static const char* UNDL = "\033[4m";
static const char* STRK = "\033[9m";

// Emit a bold-weight modifier in front of an already-painted foreground,
// since terminals treat bold+SGR as independent attribute layers.
static std::string bold_with(const std::string& fg_escape) {
    return std::string(BOLD) + fg_escape;
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
                    result += theme().md_code;
                    result += text.substr(i + 2, end - i - 2);
                    result += RST;
                    i = end + 2;
                    continue;
                }
            } else {
                size_t end = text.find('`', i + 1);
                if (end != std::string::npos) {
                    result += theme().md_code;
                    result += text.substr(i + 1, end - i - 1);
                    result += RST;
                    i = end + 1;
                    continue;
                }
            }
        }
        // Link: [text](url) — paint the visible text underlined in the
        // theme's link color.  The (url) part is dropped from the output;
        // terminals can't click on the bracket syntax anyway.
        if (text[i] == '[') {
            size_t cb = text.find(']', i + 1);
            if (cb != std::string::npos && cb + 1 < n && text[cb+1] == '(') {
                size_t cp = text.find(')', cb + 2);
                if (cp != std::string::npos) {
                    result += std::string(UNDL) + theme().md_link;
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

static bool lang_eq_ci(std::string_view lang, std::string_view want) {
    if (lang.size() != want.size()) return false;
    for (size_t i = 0; i < lang.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(lang[i]);
        const unsigned char b = static_cast<unsigned char>(want[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

bool MarkdownRenderer::is_diff_fence_lang(std::string_view lang) {
    lang = lang.substr(0, lang.find_first_of(" \t\r"));
    return lang_eq_ci(lang, "diff") || lang_eq_ci(lang, "patch");
}

static size_t fence_ltrim(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i;
}

static bool is_fence_line(const std::string& line) {
    const size_t lead = fence_ltrim(line);
    const std::string_view view(line.data() + lead, line.size() - lead);
    return view.size() >= 3 &&
           (view.substr(0, 3) == "```" || view.substr(0, 3) == "~~~");
}

std::string MarkdownRenderer::render_buffered_code_block(const std::string& close_fence) {
    std::string out;
    if (!code_open_fence_.empty()) {
        out += DIM;
        out += code_open_fence_;
        out += RST;
        out += '\n';
    }
    for (const auto& body : code_buf_) {
        out += theme().md_code;
        out += body;
        out += RST;
        out += '\n';
    }
    if (!close_fence.empty()) {
        out += DIM;
        out += close_fence;
        out += RST;
        out += '\n';
    }
    return out;
}

std::string MarkdownRenderer::process_line(const std::string& line) {
    const size_t lead = fence_ltrim(line);
    const std::string_view view(line.data() + lead, line.size() - lead);

    // Fenced code block toggle (``` or ~~~)
    if (view.size() >= 3 &&
        (view.substr(0, 3) == "```" || view.substr(0, 3) == "~~~")) {
        if (!in_code_block_) {
            in_code_block_ = true;
            in_diff_block_ = is_diff_fence_lang(view.substr(3));
            diff_buf_.clear();
            code_buf_.clear();
            code_open_fence_ = line;
            code_preview_emitted_ = false;
            return std::string{};
        }
        if (in_diff_block_) {
            if (diff_sink_ && !diff_buf_.empty()) diff_sink_(diff_buf_);
            diff_buf_.clear();
            in_diff_block_ = false;
            in_code_block_ = false;
            code_open_fence_.clear();
            return std::string{};
        }
        std::string out = render_buffered_code_block(line);
        code_buf_.clear();
        code_open_fence_.clear();
        code_preview_emitted_ = false;
        in_code_block_ = false;
        return out;
    }
    if (in_diff_block_) {
        if (!diff_buf_.empty()) diff_buf_ += '\n';
        diff_buf_ += line;
        return std::string{};
    }
    // Non-diff fenced code block body — buffer until the closing fence.
    if (in_code_block_) {
        code_buf_.push_back(line);
        if (!code_preview_emitted_ &&
            code_buf_.size() >= kCodeBlockPreviewLines) {
            code_preview_emitted_ = true;
            return std::string(DIM) + "  … (code block, " +
                   std::to_string(code_buf_.size()) + "+ lines) …" + RST + "\n";
        }
        return std::string{};
    }

    // Empty line
    if (line.empty()) return line;

    // Agent-issued command lines (/fetch, /exec, /agent, /write, /mem, /endwrite).
    // Render dim + the cmd-line color so these stand out from prose but
    // don't compete with headings.
    if (!line.empty() && line[0] == '/') {
        static const char* kCmdPrefixes[] = {
            "/fetch ", "/exec ", "/agent ", "/pane ", "/write ", "/mem ", "/endwrite", nullptr
        };
        for (auto** p = kCmdPrefixes; *p; ++p) {
            size_t plen = strlen(*p);
            if (line.size() >= plen && line.compare(0, plen, *p) == 0) {
                return theme().md_cmd_line + std::string(DIM) +
                       "> " + line + theme().md_bullet + RST + "\n";
            }
        }
    }

    // Headings: # ## ### ####  —  paint with the theme's heading[lvl-1]
    // color, bolded.  Levels 5/6 reuse the h4 slot rather than adding more.
    if (line[0] == '#') {
        size_t lvl = 0;
        while (lvl < line.size() && line[lvl] == '#') ++lvl;
        if (lvl < line.size() && line[lvl] == ' ') {
            const auto& pal = theme().md_heading;
            const std::string& color = pal[std::min(lvl - 1, pal.size() - 1)];
            std::string hashes(lvl, '#');
            std::string content = render_inline(line.substr(lvl + 1));
            return bold_with(color) + hashes + " " + content + RST;
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
            return pad + theme().md_bullet + bullet + RST + " " + content;
        }
    }

    // Indented code block (4 spaces or tab)
    if ((line.size() >= 4 && line.substr(0, 4) == "    ") ||
        (!line.empty() && line[0] == '\t')) {
        size_t skip = (line[0] == '\t') ? 1 : 4;
        return std::string("    ") + theme().md_code + line.substr(skip) + RST;
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
            const bool was_in_diff = in_diff_block_;
            const std::string line_out = process_line(line_buf_);
            if (!line_out.empty()) {
                result += line_out;
                if (line_out.back() != '\n') result += '\n';
            } else if (!was_in_diff && !in_code_block_ && line_buf_.empty()) {
                result += '\n';
            }
            line_buf_.clear();
        } else if (c != '\r') {
            line_buf_ += c;
        }
    }
    return result;
}

std::string MarkdownRenderer::flush() {
    if (!line_buf_.empty()) {
        if (is_endwrite_line(line_buf_)) { line_buf_.clear(); return {}; }
        std::string result = process_line(line_buf_);
        line_buf_.clear();
        if (result.empty() || result.back() != '\n') result += '\n';
        return result;
    }
    if (in_diff_block_ && diff_sink_ && !diff_buf_.empty()) {
        diff_sink_(diff_buf_);
        diff_buf_.clear();
        in_diff_block_ = false;
        in_code_block_ = false;
        code_open_fence_.clear();
    } else if (in_code_block_ && !code_buf_.empty()) {
        std::string result = render_buffered_code_block("");
        code_buf_.clear();
        code_open_fence_.clear();
        code_preview_emitted_ = false;
        in_code_block_ = false;
        return result;
    }
    return {};
}

void MarkdownRenderer::reset() {
    line_buf_.clear();
    in_code_block_ = false;
    in_diff_block_ = false;
    diff_buf_.clear();
    code_buf_.clear();
    code_open_fence_.clear();
    code_preview_emitted_ = false;
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
            out += RST;
        } else if (line.size() >= 3 && line.substr(0, 3) == "---") {
            out += t.accent_info;
            out += line;
            out += RST;
        } else if (line.size() >= 2 && line.substr(0, 2) == "@@") {
            out += DIM;
            out += line;
            out += RST;
        } else if (!line.empty() && line[0] == '+') {
            out += t.accent_success;
            out += line;
            out += RST;
        } else if (!line.empty() && line[0] == '-') {
            out += t.accent_error;
            out += line;
            out += RST;
        } else {
            out += t.md_code;
            out += line;
            out += RST;
        }
        out += '\n';

        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

std::string truncate_interim_output(const std::string& text,
                                      size_t max_lines,
                                      size_t max_chars) {
    std::ostringstream out;
    bool in_fence = false;
    size_t lines = 0;
    size_t chars = 0;
    bool truncated = false;

    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (is_fence_line(line)) {
            if (!in_fence) {
                in_fence = true;
                const std::string placeholder = "  … (fenced block) …";
                if (lines >= max_lines || chars + placeholder.size() > max_chars) {
                    truncated = true;
                    break;
                }
                out << placeholder << '\n';
                ++lines;
                chars += placeholder.size() + 1;
            } else {
                in_fence = false;
            }
            continue;
        }
        if (in_fence) continue;

        if (lines >= max_lines || chars + line.size() > max_chars) {
            truncated = true;
            break;
        }
        out << line << '\n';
        ++lines;
        chars += line.size() + 1;
    }

    std::string result = out.str();
    if (truncated) {
        if (!result.empty() && result.back() != '\n') result += '\n';
        result += "  … [truncated — full result in synthesis turn]\n";
    }
    return result;
}

} // namespace arbiter
