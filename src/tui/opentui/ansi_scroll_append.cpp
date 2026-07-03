#include "tui/opentui/ansi_scroll_append.h"

#include "tui/opentui/engine.h"

#include <cctype>
#include <cstring>
#include <vector>

namespace arbiter::opentui {

namespace {

constexpr std::uint32_t kAttrBold          = 1u << 0;
constexpr std::uint32_t kAttrDim           = 1u << 1;
constexpr std::uint32_t kAttrItalic        = 1u << 2;
constexpr std::uint32_t kAttrUnderline     = 1u << 3;
constexpr std::uint32_t kAttrStrikethrough = 1u << 7;

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
    while (i > 0 && is_continuation(static_cast<unsigned char>(text[i - 1]))) --i;
    if (i == text.size()) return;

    const unsigned char lead = static_cast<unsigned char>(text[i]);
    const int need = utf8_code_unit_count(lead);
    if (need <= 1) return;

    const size_t have = text.size() - i;
    if (have >= static_cast<size_t>(need)) return;

    hold.append(text.begin() + static_cast<std::ptrdiff_t>(i), text.end());
    text.resize(i);
}

std::vector<int> parse_sgr_params(std::string_view body) {
    std::vector<int> out;
    if (body.empty()) {
        out.push_back(0);
        return out;
    }
    int cur = 0;
    bool have = false;
    for (char c : body) {
        if (c == ';') {
            out.push_back(have ? cur : 0);
            cur = 0;
            have = false;
        } else if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            have = true;
        }
    }
    out.push_back(have ? cur : 0);
    return out;
}

std::string strip_readline_markers(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == 0x01) {
            ++i;
            while (i < raw.size() && static_cast<unsigned char>(raw[i]) != 0x02) ++i;
            if (i < raw.size()) ++i;
            continue;
        }
        if (c < 0x20 && c != '\n' && c != '\t' && c != '\r') {
            ++i;
            continue;
        }
        out.push_back(static_cast<char>(c));
        ++i;
    }
    return out;
}

bool is_terminal_response_csi(std::string_view params, char final) {
    if (final == 'R') return true;
    if (final == 'c' && !params.empty()
        && (params[0] == '?' || params[0] == '>' || params[0] == '=')) {
        return true;
    }
    if (final == 'y' && params.find('$') != std::string_view::npos) return true;
    if (final == 'u' && !params.empty() && params[0] == '?') return true;
    return false;
}

// Skip a bare CSI (no leading ESC) when it is a terminal reply such as [1;1R.
size_t skip_bare_csi_response(std::string_view chunk, size_t i) {
    if (i >= chunk.size() || chunk[i] != '[') return 0;
    size_t j = i + 1;
    std::string params;
    while (j < chunk.size()) {
        const unsigned char x = static_cast<unsigned char>(chunk[j]);
        if ((x >= '0' && x <= '9') || x == ';' || x == '?' || x == '$'
            || x == '>' || x == '=') {
            params.push_back(static_cast<char>(x));
            ++j;
            continue;
        }
        if (x >= 0x40 && x <= 0x7E) {
            const char final = static_cast<char>(x);
            if (is_terminal_response_csi(params, final)) return j + 1;
            return 0;
        }
        return 0;
    }
    return 0;
}

size_t skip_bare_osc(std::string_view chunk, size_t i) {
    if (i >= chunk.size() || chunk[i] != ']') return 0;
    size_t j = i + 1;
    while (j < chunk.size()) {
        if (chunk[j] == 0x07) return j + 1;
        if (chunk[j] == 0x1B && j + 1 < chunk.size() && chunk[j + 1] == '\\') {
            return j + 2;
        }
        ++j;
    }
    return 0;
}

} // namespace

bool AnsiScrollAppender::StyleKey::operator==(const StyleKey& o) const {
    return has_fg == o.has_fg && has_bg == o.has_bg && attrs == o.attrs
        && fg == o.fg && bg == o.bg;
}

std::size_t AnsiScrollAppender::StyleKeyHash::operator()(const StyleKey& k) const {
    std::size_t h = k.attrs;
    h ^= (k.has_fg ? 0x9e3779b9 : 0) + (h << 6) + (h >> 2);
    h ^= (k.has_bg ? 0x85ebca6b : 0) + (h << 6) + (h >> 2);
    for (std::uint16_t v : k.fg) h ^= static_cast<std::size_t>(v) + (h << 6) + (h >> 2);
    for (std::uint16_t v : k.bg) h ^= static_cast<std::size_t>(v) + (h << 6) + (h >> 2);
    return h;
}

std::array<std::uint16_t, 4> AnsiScrollAppender::pack_rgb(std::uint8_t r,
                                                            std::uint8_t g,
                                                            std::uint8_t b) {
    return rgba8(r, g, b);
}

AnsiScrollAppender::AnsiScrollAppender(OpenTuiHandle buffer) : buffer_(buffer) {
    syntax_ = createSyntaxStyle();
    if (syntax_ != 0 && buffer_ != 0) {
        textBufferSetSyntaxStyle(buffer_, syntax_);
    }
}

AnsiScrollAppender::~AnsiScrollAppender() {
    if (syntax_ != 0) destroySyntaxStyle(syntax_);
}

void AnsiScrollAppender::clear() {
    esc_hold_.clear();
    utf8_hold_.clear();
    plain_storage_.clear();
    fg_.reset();
    bg_.reset();
    attrs_ = 0;
    if (buffer_ != 0) {
        textBufferClearAllHighlights(buffer_);
        textBufferClear(buffer_);
    }
}

void AnsiScrollAppender::append(std::string_view raw) {
    if (buffer_ == 0 || raw.empty()) return;
    std::string combined;
    combined.reserve(esc_hold_.size() + utf8_hold_.size() + raw.size());
    combined.append(esc_hold_);
    esc_hold_.clear();
    combined.append(utf8_hold_);
    utf8_hold_.clear();
    combined.append(raw);
    feed_bytes(combined);
}

void AnsiScrollAppender::feed_bytes(std::string_view chunk) {
    std::string plain_run;
    plain_run.reserve(chunk.size());

    auto flush_plain = [&]() {
        if (!plain_run.empty()) {
            emit_plain(plain_run);
            plain_run.clear();
        }
    };

    size_t i = 0;
    while (i < chunk.size()) {
        const unsigned char c = static_cast<unsigned char>(chunk[i]);
        if (c == 0x1B) {
            if (i + 1 >= chunk.size()) {
                esc_hold_.assign(chunk.data() + i, chunk.size() - i);
                flush_plain();
                return;
            }
            const char next = chunk[i + 1];
            if (next == '[') {
                size_t j = i + 2;
                std::string params;
                while (j < chunk.size()) {
                    const unsigned char x = static_cast<unsigned char>(chunk[j]);
                    if ((x >= '0' && x <= '9') || x == ';' || x == '?' || x == '$'
                        || x == '>' || x == '=') {
                        params.push_back(static_cast<char>(x));
                        ++j;
                        continue;
                    }
                    if (x >= 0x40 && x <= 0x7E) break;
                    ++j;
                }
                if (j >= chunk.size()) {
                    esc_hold_.assign(chunk.data() + i, chunk.size() - i);
                    flush_plain();
                    return;
                }
                const char final = chunk[j];
                if (final == 'm') {
                    flush_plain();
                    apply_sgr(parse_sgr_params(params));
                }
                i = j + 1;
                continue;
            }
            if (next == ']' || next == 'P') {
                flush_plain();
                size_t j = i + 2;
                while (j < chunk.size()) {
                    if (chunk[j] == 0x07) { j++; break; }
                    if (chunk[j] == 0x1B && j + 1 < chunk.size() && chunk[j + 1] == '\\') {
                        j += 2;
                        break;
                    }
                    ++j;
                }
                if (j >= chunk.size()) {
                    esc_hold_.assign(chunk.data() + i, chunk.size() - i);
                    return;
                }
                i = j;
                continue;
            }
            flush_plain();
            i += 2;
            continue;
        }
        if (const size_t bare_csi = skip_bare_csi_response(chunk, i); bare_csi > i) {
            flush_plain();
            i = bare_csi;
            continue;
        }
        if (chunk[i] == ']') {
            flush_plain();
            if (const size_t end = skip_bare_osc(chunk, i); end > i) {
                i = end;
                continue;
            }
            esc_hold_.assign(chunk.data() + i, chunk.size() - i);
            return;
        }
        if (c == 0x01) {
            flush_plain();
            ++i;
            while (i < chunk.size() && static_cast<unsigned char>(chunk[i]) != 0x02) ++i;
            if (i < chunk.size()) ++i;
            continue;
        }
        plain_run.push_back(static_cast<char>(c));
        ++i;
    }

    if (!plain_run.empty()) {
        peel_incomplete_utf8(plain_run, utf8_hold_);
    }
    flush_plain();
}

void AnsiScrollAppender::apply_sgr(const std::vector<int>& params) {
    for (size_t i = 0; i < params.size(); ++i) {
        const int p = params[i];
        if (p == 0) {
            fg_.reset();
            bg_.reset();
            attrs_ = 0;
            continue;
        }
        if (p == 1) { attrs_ |= kAttrBold; continue; }
        if (p == 2) { attrs_ |= kAttrDim; continue; }
        if (p == 3) { attrs_ |= kAttrItalic; continue; }
        if (p == 4) { attrs_ |= kAttrUnderline; continue; }
        if (p == 9) { attrs_ |= kAttrStrikethrough; continue; }
        if (p == 22) { attrs_ &= ~(kAttrBold | kAttrDim); continue; }
        if (p == 23) { attrs_ &= ~kAttrItalic; continue; }
        if (p == 24) { attrs_ &= ~kAttrUnderline; continue; }
        if (p == 29) { attrs_ &= ~kAttrStrikethrough; continue; }
        if (p == 39) { fg_.reset(); continue; }
        if (p == 49) { bg_.reset(); continue; }
        if (p == 38 && i + 2 < params.size() && params[i + 1] == 5) {
            i += 2;
            continue;
        }
        if (p == 48 && i + 2 < params.size() && params[i + 1] == 5) {
            i += 2;
            continue;
        }
        if (p == 38 && i + 4 < params.size() && params[i + 1] == 2) {
            fg_ = pack_rgb(static_cast<std::uint8_t>(params[i + 2]),
                           static_cast<std::uint8_t>(params[i + 3]),
                           static_cast<std::uint8_t>(params[i + 4]));
            i += 4;
            continue;
        }
        if (p == 48 && i + 4 < params.size() && params[i + 1] == 2) {
            bg_ = pack_rgb(static_cast<std::uint8_t>(params[i + 2]),
                           static_cast<std::uint8_t>(params[i + 3]),
                           static_cast<std::uint8_t>(params[i + 4]));
            i += 4;
            continue;
        }
    }
}

void AnsiScrollAppender::emit_plain(std::string_view text) {
    if (text.empty()) return;
    plain_storage_.push_back(strip_readline_markers(text));
    const std::string& cleaned = plain_storage_.back();
    if (cleaned.empty()) {
        plain_storage_.pop_back();
        return;
    }

    const std::uint32_t start = textBufferGetLength(buffer_);
    textBufferAppend(buffer_, cleaned.data(), static_cast<std::uint32_t>(cleaned.size()));
    const std::uint32_t end = textBufferGetLength(buffer_);
    if (end <= start) return;

    const std::uint32_t style_id = style_id_for_current();
    if (style_id == 0) return;

    OpenTuiHighlight hl{};
    hl.start = start;
    hl.end = end;
    hl.style_id = style_id;
    hl.priority = 1;
    hl.hl_ref = 0;
    textBufferAddHighlightByCharRange(buffer_, &hl);
}

std::uint32_t AnsiScrollAppender::style_id_for_current() {
    if (!fg_ && !bg_ && attrs_ == 0) return 0;

    StyleKey key;
    key.attrs = attrs_;
    if (fg_) {
        key.has_fg = true;
        key.fg = *fg_;
    }
    if (bg_) {
        key.has_bg = true;
        key.bg = *bg_;
    }

    if (const auto it = style_cache_.find(key); it != style_cache_.end()) {
        return it->second;
    }

    char name_buf[32];
    std::snprintf(name_buf, sizeof(name_buf), "md%u", next_style_name_++);

    const std::uint32_t id = syntaxStyleRegister(
        syntax_,
        name_buf,
        static_cast<std::uint32_t>(std::strlen(name_buf)),
        key.has_fg ? key.fg.data() : nullptr,
        key.has_bg ? key.bg.data() : nullptr,
        key.attrs);
    if (id != 0) style_cache_.emplace(key, id);
    return id;
}

} // namespace arbiter::opentui
