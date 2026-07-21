#include "styled_text.h"

#include "theme.h"
#include "tui/tui_design.h"

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
    case StyleId::WritLine:  return t.bold + t.md_cmd_line;
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
    case StyleId::UserEchoArrow:return t.user_echo_bg + t.user_echo_arrow;
    case StyleId::UserEchoText: return t.user_echo_bg + t.user_echo_text;
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
    // No caret — differentiation is the input-matching background strip.
    if (text.empty()) {
        // Zero-width span so empty turns still pad/paint the bg strip.
        line.spans.push_back({0, 0, StyleId::UserEchoText});
        return line;
    }
    styled_append(line, StyleId::UserEchoText, text);
    return line;
}

std::vector<StyledLine> styled_user_echo_lines(std::string_view text) {
    std::vector<StyledLine> body;
    // Normalize CRLF / bare CR so `\` continuations and pasted text split cleanly.
    std::string normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') continue;
            normalized.push_back('\n');
            continue;
        }
        normalized.push_back(text[i]);
    }
    if (normalized.empty()) {
        body.push_back(styled_user_echo({}));
    } else {
        size_t start = 0;
        while (start <= normalized.size()) {
            const size_t nl = normalized.find('\n', start);
            if (nl == std::string::npos) {
                body.push_back(styled_user_echo(std::string_view(normalized).substr(start)));
                break;
            }
            body.push_back(styled_user_echo(
                std::string_view(normalized).substr(start, nl - start)));
            start = nl + 1;
            if (start == normalized.size()) {
                // Trailing newline is absorbed into the bottom vertical pad.
                break;
            }
        }
    }
    // Drop a lone trailing blank body row — vertical pads own that chrome.
    while (!body.empty() && body.back().text.empty()) body.pop_back();
    if (body.empty()) body.push_back(styled_user_echo({}));

    // Vertical chrome is added when the contiguous echo run is rendered.
    // Keeping source lines payload-only avoids applying the top/bottom pads
    // again during replay, resize, or retheme.
    return body;
}

bool is_styled_user_echo_line(const StyledLine& line) {
    if (line.spans.empty()) return false;
    for (const StyleSpan& span : line.spans) {
        if (span.id != StyleId::UserEchoText && span.id != StyleId::UserEchoArrow) {
            return false;
        }
    }
    return true;
}

bool is_user_echo_find_command(const StyledLine& line) {
    if (!is_styled_user_echo_line(line)) return false;
    std::string_view payload = line.text;
    while (!payload.empty() && payload.back() == ' ') payload.remove_suffix(1);
    if (payload.empty()) return false;  // vertical pad rows
    static constexpr char kFind[] = "/find";
    static constexpr size_t kFindLen = sizeof(kFind) - 1;
    if (payload.size() < kFindLen) return false;
    for (size_t i = 0; i < kFindLen; ++i) {
        const char c = payload[i];
        const char expect = kFind[i];
        const char lower = (c >= 'A' && c <= 'Z')
            ? static_cast<char>(c - 'A' + 'a') : c;
        if (lower != expect) return false;
    }
    return payload.size() == kFindLen || payload[kFindLen] == ' ';
}

StyledLine pad_styled_user_echo_line(const StyledLine& line, int cols) {
    const int width = cols < 1 ? 1 : cols;
    const TuiDesign& d = tui_design();
    const int inset = std::max(0, d.layout.header_padding_x);
    const int first_row_budget = std::max(0, width - inset);

    // Expects an unpadded source line (payload only). Emit chrome is applied here.
    std::string body = line.text;

    StyledLine out;
    if (inset > 0) {
        styled_append(out, StyleId::UserEchoText,
                      std::string(static_cast<size_t>(inset), ' '));
    }

    int bw = static_cast<int>(display_width(body));
    if (bw <= first_row_budget) {
        body.append(static_cast<size_t>(first_row_budget - bw), ' ');
    } else {
        // Long turns wrap in the text buffer; pad so the final visual row
        // fills to `width` (inset occupies the start of row 0 only).
        const int total = inset + bw;
        const int rem = total % width;
        if (rem != 0) {
            body.append(static_cast<size_t>(width - rem), ' ');
        }
    }
    if (body.empty() && out.text.empty()) {
        out.spans.push_back({0, 0, StyleId::UserEchoText});
        return out;
    }
    styled_append(out, StyleId::UserEchoText, body);
    return out;
}

std::vector<StyledLine> wrap_pad_styled_user_echo_line(const StyledLine& line, int cols) {
    const int width = cols < 1 ? 1 : cols;
    // One cell of breathing room on each side when the band is wide enough.
    const int hpad = (width >= 3) ? 1 : 0;
    const int content_cols = width - 2 * hpad;

    std::string body = line.text;
    // Same over-budget trailing-space trim as pad_styled_user_echo_line.
    while (!body.empty() && body.back() == ' '
           && static_cast<int>(display_width(body)) > content_cols) {
        body.pop_back();
    }

    std::vector<StyledLine> out;
    auto flush_row = [&](std::string row) {
        // Pad the content slice, then wrap with horizontal inset spaces so
        // glyphs never sit on the band edge.
        StyledLine content = styled_user_echo(row);
        const int row_w = static_cast<int>(display_width(content.text));
        if (row_w < content_cols) {
            content.text.append(static_cast<size_t>(content_cols - row_w), ' ');
            content.spans.clear();
            content.spans.push_back(
                {0, static_cast<std::uint32_t>(content.text.size()),
                 StyleId::UserEchoText});
        }
        if (hpad == 0) {
            out.push_back(std::move(content));
            return;
        }
        std::string band(static_cast<size_t>(hpad), ' ');
        band += content.text;
        band.append(static_cast<size_t>(hpad), ' ');
        out.push_back(styled_user_echo(band));
    };

    if (body.empty()) {
        flush_row({});
        return out;
    }

    // Greedy word wrap into the inset content width. Emitting one buffer line
    // per visual row (each padded to `width`) means OpenTUI word-wrap cannot
    // leave short rows with glyph-only backgrounds.
    std::string row;
    size_t i = 0;
    while (i < body.size()) {
        const bool is_space = body[i] == ' ';
        size_t j = i + 1;
        if (is_space) {
            while (j < body.size() && body[j] == ' ') ++j;
        } else {
            while (j < body.size() && body[j] != ' ') ++j;
        }
        const std::string_view tok(body.data() + i, j - i);
        const int tok_w = static_cast<int>(display_width(tok));
        const int row_w = static_cast<int>(display_width(row));

        if (!row.empty() && row_w + tok_w > content_cols) {
            if (is_space) {
                // Whitespace that caused the wrap is the break point — drop it.
                flush_row(std::move(row));
                row.clear();
                i = j;
                continue;
            }
            flush_row(std::move(row));
            row.clear();
        }

        if (tok_w > content_cols) {
            std::string remain(tok);
            while (!remain.empty()) {
                const int room = content_cols - static_cast<int>(display_width(row));
                if (room < 1) {
                    flush_row(std::move(row));
                    row.clear();
                    continue;
                }
                std::string chunk = trim_to_display_cols(remain, room);
                if (chunk.empty()) chunk.assign(remain, 0, 1);
                row += chunk;
                remain.erase(0, chunk.size());
                if (static_cast<int>(display_width(row)) >= content_cols) {
                    flush_row(std::move(row));
                    row.clear();
                }
            }
        } else {
            row.append(tok.data(), tok.size());
            if (static_cast<int>(display_width(row)) >= content_cols) {
                flush_row(std::move(row));
                row.clear();
            }
        }
        i = j;
    }
    if (!row.empty() || out.empty()) flush_row(std::move(row));
    return out;
}

std::vector<StyledLine> wrap_pad_styled_user_echo_block(
    const std::vector<StyledLine>& lines, int cols) {
    const int width = cols < 1 ? 1 : cols;
    std::vector<StyledLine> out;
    const StyledLine blank = pad_styled_user_echo_line(styled_user_echo({}), width);
    out.push_back(blank);
    for (const StyledLine& line : lines) {
        if (!is_styled_user_echo_line(line)) continue;
        auto rows = wrap_pad_styled_user_echo_line(line, width);
        out.insert(out.end(),
                   std::make_move_iterator(rows.begin()),
                   std::make_move_iterator(rows.end()));
    }
    if (out.size() == 1) {
        // No content rows survived — still emit a text row between pads.
        out.push_back(blank);
    }
    out.push_back(blank);
    return out;
}

bool resize_styled_user_echo_lines(std::vector<StyledLine>& lines, int cols) {
    bool changed = false;
    for (StyledLine& line : lines) {
        if (!is_styled_user_echo_line(line)) continue;
        StyledLine next = pad_styled_user_echo_line(line, cols);
        if (next.text == line.text) continue;
        line = std::move(next);
        changed = true;
    }
    return changed;
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
