#include "styled_text.h"

#include "theme.h"

#include <cwchar>
#include <string>

namespace arbiter {

namespace {

// Thread-local scratch for style_open — theme() strings must outlive the call.
thread_local std::string g_style_scratch;

int utf8_decode(std::string_view text, size_t& index) {
    if (index >= text.size()) return -1;
    const unsigned char c0 = static_cast<unsigned char>(text[index]);
    if (c0 < 0x80) {
        ++index;
        return static_cast<int>(c0);
    }
    if ((c0 & 0xE0) == 0xC0 && index + 1 < text.size()) {
        const unsigned char c1 = static_cast<unsigned char>(text[index + 1]);
        if ((c1 & 0xC0) != 0x80) { ++index; return 0xFFFD; }
        const int cp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
        index += 2;
        return cp;
    }
    if ((c0 & 0xF0) == 0xE0 && index + 2 < text.size()) {
        const unsigned char c1 = static_cast<unsigned char>(text[index + 1]);
        const unsigned char c2 = static_cast<unsigned char>(text[index + 2]);
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) { ++index; return 0xFFFD; }
        const int cp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
        index += 3;
        return cp;
    }
    if ((c0 & 0xF8) == 0xF0 && index + 3 < text.size()) {
        const unsigned char c1 = static_cast<unsigned char>(text[index + 1]);
        const unsigned char c2 = static_cast<unsigned char>(text[index + 2]);
        const unsigned char c3 = static_cast<unsigned char>(text[index + 3]);
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
            ++index;
            return 0xFFFD;
        }
        const int cp = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) |
                       ((c2 & 0x3F) << 6) | (c3 & 0x3F);
        index += 4;
        return cp;
    }
    ++index;
    return 0xFFFD;
}

std::string style_open(StyleId id) {
    const Theme& t = theme();
    switch (id) {
    case StyleId::Default:   return {};
    case StyleId::Dim:       return t.dim;
    case StyleId::Bold:      return t.bold;
    case StyleId::Italic:    return t.italic;
    case StyleId::Strike:    return t.strike + t.dim;
    case StyleId::Heading1:  return t.bold + t.md_heading[0];
    case StyleId::Heading2:  return t.bold + t.md_heading[1];
    case StyleId::Heading3:  return t.bold + t.md_heading[2];
    case StyleId::Heading4:  return t.bold + t.md_heading[3];
    case StyleId::Code:
    case StyleId::CodeFence: return t.md_code;
    case StyleId::Link:      return t.underline + t.md_link;
    case StyleId::Bullet:    return t.md_bullet;
    case StyleId::Blockquote:return t.dim;
    case StyleId::Rule:      return t.dim + t.md_rule;
    case StyleId::WritLine:  return t.md_cmd_line + t.dim;
    case StyleId::DiffAdd:   return t.accent_success;
    case StyleId::DiffRemove:return t.accent_error;
    case StyleId::DiffHunk:  return t.dim;
    case StyleId::DiffFile:  return t.accent_info;
    case StyleId::Success:   return t.accent_success;
    case StyleId::Error:     return t.accent_error;
    case StyleId::Warning:   return t.accent_warning;
    case StyleId::Info:      return t.accent_info;
    case StyleId::CodeKeyword:  return t.md_code_keyword;
    case StyleId::CodeString:   return t.md_code_string;
    case StyleId::CodeComment:  return t.dim + t.md_code_comment;
    case StyleId::CodeNumber:   return t.md_code_number;
    case StyleId::CodeType:     return t.md_code_type;
    case StyleId::CodeFunction: return t.md_code_function;
    case StyleId::System:       return t.dim + t.system_fg;
    case StyleId::UserEchoArrow:return t.user_echo_arrow;
    case StyleId::UserEchoText: return t.user_echo_text;
    }
    return {};
}

} // namespace

std::size_t display_width(std::string_view text) {
    std::size_t cols = 0;
    size_t i = 0;
    while (i < text.size()) {
        const int cp = utf8_decode(text, i);
        if (cp < 0) break;
        wchar_t wc = static_cast<wchar_t>(cp);
        int w = ::wcwidth(wc);
        if (w < 0) w = 1;
        cols += static_cast<std::size_t>(w);
    }
    return cols;
}

std::string trim_to_display_cols(std::string s, int max_cols) {
    if (max_cols <= 0) return {};
    while (!s.empty() && static_cast<int>(display_width(s)) > max_cols) {
        s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
            s.pop_back();
        }
    }
    return s;
}

void styled_append(StyledLine& line, StyleId id, std::string_view text) {
    if (text.empty()) return;
    const std::uint32_t begin = static_cast<std::uint32_t>(line.text.size());
    line.text.append(text);
    line.spans.push_back({begin, static_cast<std::uint32_t>(line.text.size()), id});
}

void styled_append_char(StyledLine& line, StyleId id, char c) {
    styled_append(line, id, std::string_view(&c, 1));
}

StyledLine styled_user_echo(std::string_view text) {
    StyledLine line;
    styled_append(line, StyleId::UserEchoArrow, "> ");
    styled_append(line, StyleId::UserEchoText, text);
    return line;
}

bool is_styled_rule_line(const StyledLine& line) {
    if (line.text.empty()) return false;
    for (unsigned char c : line.text) {
        if (c != '-') return false;
    }
    if (line.spans.empty()) return false;
    for (const StyleSpan& span : line.spans) {
        if (span.id != StyleId::Rule) return false;
    }
    return true;
}

StyledLine styled_rule_line(int cols) {
    StyledLine line;
    const int width = cols < 1 ? 1 : cols;
    styled_append(line, StyleId::Rule, std::string(static_cast<size_t>(width), '-'));
    return line;
}

bool resize_styled_rule_lines(std::vector<StyledLine>& lines, int cols) {
    const int width = cols < 1 ? 1 : cols;
    bool changed = false;
    for (StyledLine& line : lines) {
        if (!is_styled_rule_line(line)) continue;
        if (static_cast<int>(line.text.size()) == width) continue;
        line = styled_rule_line(width);
        changed = true;
    }
    return changed;
}

std::string to_ansi(const StyledLine& line) {
    if (line.text.empty()) return {};
    const Theme& t = theme();
    std::string out;
    if (line.spans.empty()) {
        out += line.text;
        return out;
    }
    std::size_t cursor = 0;
    for (const StyleSpan& span : line.spans) {
        if (span.begin > line.text.size()) break;
        if (span.begin > cursor) {
            out += line.text.substr(cursor, span.begin - cursor);
        }
        const std::size_t end = std::min(static_cast<std::size_t>(span.end), line.text.size());
        if (end <= span.begin) continue;
        out += style_open(span.id);
        out += line.text.substr(span.begin, end - span.begin);
        if (span.id != StyleId::Default) out += t.reset;
        cursor = end;
    }
    if (cursor < line.text.size()) out += line.text.substr(cursor);
    return out;
}

std::string styled_lines_to_ansi(const std::vector<StyledLine>& lines) {
    std::string out;
    for (const StyledLine& line : lines) {
        const std::string rendered = to_ansi(line);
        if (rendered.empty()) {
            out += '\n';
            continue;
        }
        out += rendered;
        if (out.back() != '\n') out += '\n';
    }
    return out;
}

} // namespace arbiter
