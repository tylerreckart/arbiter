#include "tui/opentui/span_scroll_append.h"

#include "tui/opentui/engine.h"
#include "tui/style_resolver.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace arbiter::opentui {

namespace {

constexpr std::size_t kMaxStoredRuns = 20000;
constexpr std::size_t kCompactedRuns = 16000;

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

} // namespace

bool SpanScrollAppender::StyleKey::operator==(const StyleKey& o) const {
    return has_fg == o.has_fg && has_bg == o.has_bg && attrs == o.attrs &&
           (!has_fg || fg == o.fg) && (!has_bg || bg == o.bg);
}

std::size_t SpanScrollAppender::StyleKeyHash::operator()(const StyleKey& k) const {
    std::size_t h = k.attrs;
    if (k.has_fg) {
        h ^= static_cast<std::size_t>(k.fg[0]) << 1;
        h ^= static_cast<std::size_t>(k.fg[1]) << 2;
        h ^= static_cast<std::size_t>(k.fg[2]) << 3;
    }
    if (k.has_bg) {
        h ^= static_cast<std::size_t>(k.bg[0]) << 4;
        h ^= static_cast<std::size_t>(k.bg[1]) << 5;
        h ^= static_cast<std::size_t>(k.bg[2]) << 6;
    }
    return h;
}

SpanScrollAppender::SpanScrollAppender(OpenTuiHandle buffer) : buffer_(buffer) {
    syntax_ = createSyntaxStyle();
    if (syntax_ != 0) textBufferSetSyntaxStyle(buffer_, syntax_);
}

SpanScrollAppender::~SpanScrollAppender() {
    if (syntax_ != 0) destroySyntaxStyle(syntax_);
}

void SpanScrollAppender::clear() {
    plain_storage_.clear();
    style_cache_.clear();
    next_style_name_ = 0;
    utf8_hold_.clear();
    if (buffer_ != 0) {
        textBufferReset(buffer_);
        if (syntax_ != 0) textBufferSetSyntaxStyle(buffer_, syntax_);
    }
}

SpanScrollAppender::StyleKey SpanScrollAppender::style_key_for(StyleId id) const {
    const ResolvedStyle rs = resolve_style(id);
    StyleKey key;
    key.attrs = rs.attrs;
    if (rs.fg) {
        key.has_fg = true;
        key.fg = *rs.fg;
    }
    if (rs.bg) {
        key.has_bg = true;
        key.bg = *rs.bg;
    }
    return key;
}

void SpanScrollAppender::emit_plain(std::string_view text, StyleId id) {
    if (text.empty()) return;

    StoredRun run;
    run.text.assign(text);
    run.style = style_key_for(id);
    run.has_style = run.style.has_fg || run.style.has_bg || run.style.attrs != 0;
    plain_storage_.push_back(std::move(run));
    const StoredRun& stored = plain_storage_.back();

    const std::uint32_t start = textBufferGetLength(buffer_);
    textBufferAppend(buffer_,
                     stored.text.data(),
                     static_cast<std::uint32_t>(stored.text.size()));
    const std::uint32_t end = textBufferGetLength(buffer_);
    if (end > start && stored.has_style) {
        add_highlight(start, end, stored.style);
    }
    compact_storage_if_needed();
}

void SpanScrollAppender::add_highlight(std::uint32_t start,
                                       std::uint32_t end,
                                       const StyleKey& key) {
    if (end <= start) return;
    const std::uint32_t style_id = style_id_for_key(key);
    if (style_id == 0) return;
    OpenTuiHighlight hl{};
    hl.start = start;
    hl.end = end;
    hl.style_id = style_id;
    hl.priority = 1;
    hl.hl_ref = 0;
    textBufferAddHighlightByCharRange(buffer_, &hl);
}

std::uint32_t SpanScrollAppender::style_id_for_key(const StyleKey& key) {
    if (!key.has_fg && !key.has_bg && key.attrs == 0) return 0;
    if (const auto it = style_cache_.find(key); it != style_cache_.end()) {
        return it->second;
    }

    char name_buf[32];
    std::snprintf(name_buf, sizeof(name_buf), "sp%u", next_style_name_++);

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

void SpanScrollAppender::append_line(const StyledLine& line) {
    if (buffer_ == 0) return;

    if (line.spans.empty()) {
        std::string chunk = utf8_hold_;
        utf8_hold_.clear();
        chunk += line.text;
        peel_incomplete_utf8(chunk, utf8_hold_);
        emit_plain(chunk, StyleId::Default);
    } else {
        std::size_t cursor = 0;
        for (const StyleSpan& span : line.spans) {
            if (span.begin > line.text.size()) break;
            if (span.begin > cursor) {
                emit_plain(line.text.substr(cursor, span.begin - cursor), StyleId::Default);
            }
            const std::size_t end = std::min(static_cast<std::size_t>(span.end), line.text.size());
            if (end > span.begin) {
                emit_plain(line.text.substr(span.begin, end - span.begin), span.id);
            }
            cursor = end;
        }
        if (cursor < line.text.size()) {
            emit_plain(line.text.substr(cursor), StyleId::Default);
        }
    }

    emit_plain("\n", StyleId::Default);
}

void SpanScrollAppender::compact_storage_if_needed() {
    if (plain_storage_.size() <= kMaxStoredRuns) return;
    while (plain_storage_.size() > kCompactedRuns) plain_storage_.pop_front();
    rebuild_buffer_from_storage();
}

void SpanScrollAppender::rebuild_buffer_from_storage() {
    if (buffer_ == 0) return;
    textBufferReset(buffer_);
    if (syntax_ != 0) textBufferSetSyntaxStyle(buffer_, syntax_);

    for (const StoredRun& run : plain_storage_) {
        if (run.text.empty()) continue;
        const std::uint32_t start = textBufferGetLength(buffer_);
        textBufferAppend(buffer_,
                         run.text.data(),
                         static_cast<std::uint32_t>(run.text.size()));
        const std::uint32_t end = textBufferGetLength(buffer_);
        if (run.has_style) add_highlight(start, end, run.style);
    }
}

} // namespace arbiter::opentui
