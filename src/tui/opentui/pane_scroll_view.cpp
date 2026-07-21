#include "tui/opentui/pane_scroll_view.h"

#include "code_highlighter.h"
#include "markdown.h"
#include "styled_text.h"
#include "tui/ansi_util.h"
#include "tui/opentui/rounded_box.h"
#include "tui/style_resolver.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <memory>
#include <stdexcept>

namespace arbiter::opentui {

namespace {

constexpr std::uint8_t kWrapWord = 2;

int cell_width(std::string_view s) {
    return static_cast<int>(arbiter::display_width(s));
}

std::string trim_to_cells(std::string s, int max_cells) {
    return arbiter::trim_to_display_cols(std::move(s), max_cells);
}

std::vector<StyledLine> prepare_prose_lines(std::vector<StyledLine> lines,
                                            int wrap_cols,
                                            int paragraph_gap,
                                            int trailing_empties) {
    std::vector<StyledLine> out;
    out.reserve(lines.size());
    int empties = trailing_empties;
    for (StyledLine& line : lines) {
        if (arbiter::is_styled_rule_line(line)) {
            empties = 0;
            out.push_back(arbiter::styled_rule_line(wrap_cols));
            continue;
        }
        if (arbiter::is_styled_user_echo_line(line)) {
            // Keep source unpadded so trailing spaces survive wrap resize;
            // ProseSegment pads at emit time.
            empties = 0;
            out.push_back(std::move(line));
            continue;
        }
        if (line.text.empty()) {
            if (empties < paragraph_gap) {
                out.push_back(std::move(line));
                ++empties;
            }
            continue;
        }
        empties = 0;
        out.push_back(std::move(line));
    }
    return out;
}

int trailing_empty_count(const std::vector<StyledLine>& lines) {
    int n = 0;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (!it->text.empty()) break;
        ++n;
    }
    return n;
}

void fill_rect(OpenTuiHandle frame,
               int x,
               int y,
               int w,
               int h,
               const TuiRgba& bg) {
    if (w <= 0 || h <= 0 || x < 0 || y < 0) return;
    bufferFillRect(frame,
                   static_cast<std::uint32_t>(x),
                   static_cast<std::uint32_t>(y),
                   static_cast<std::uint32_t>(w),
                   static_cast<std::uint32_t>(h),
                   bg.data());
}

void draw_text(OpenTuiHandle frame,
               int x,
               int y,
               std::string_view text,
               const TuiRgba& fg,
               const TuiRgba& bg,
               std::uint32_t attrs = 0) {
    if (text.empty() || x < 0 || y < 0) return;
    bufferDrawText(frame,
                   text.data(),
                   static_cast<std::uint32_t>(text.size()),
                   static_cast<std::uint32_t>(x),
                   static_cast<std::uint32_t>(y),
                   fg.data(),
                   bg.data(),
                   attrs);
}

} // namespace

// --- ProseSegment (span-native scrollback) ------------------------------------

PaneScrollView::ProseSegment::ProseSegment() {
    buffer_ = createTextBuffer(/*width_method wcwidth=*/0);
    if (buffer_ == 0) throw std::runtime_error("createTextBuffer failed");
    view_ = createTextBufferView(buffer_);
    if (view_ == 0) {
        destroyTextBuffer(buffer_);
        buffer_ = 0;
        throw std::runtime_error("createTextBufferView failed");
    }
    textBufferViewSetWrapMode(view_, kWrapWord);
    textBufferViewSetFirstLineOffset(view_, 0);
    span_append_ = std::make_unique<SpanScrollAppender>(buffer_);
}

PaneScrollView::ProseSegment::~ProseSegment() {
    if (view_ != 0) destroyTextBufferView(view_);
    if (buffer_ != 0) destroyTextBuffer(buffer_);
}

void PaneScrollView::ProseSegment::emit_line(const StyledLine& line) {
    if (!span_append_) return;
    if (arbiter::is_styled_user_echo_line(line)) {
        // Prefer emit_echo_run for contiguous blocks (vertical pad). Single
        // lines still get full-width fill + horizontal inset.
        for (const StyledLine& row :
             arbiter::wrap_pad_styled_user_echo_line(line, wrap_cols_)) {
            span_append_->append_line(row);
        }
    } else {
        span_append_->append_line(line);
    }
}

void PaneScrollView::ProseSegment::emit_echo_run(
    const StyledLine* begin, const StyledLine* end) {
    if (!span_append_ || begin >= end) return;
    std::vector<StyledLine> block(begin, end);
    for (const StyledLine& row :
         arbiter::wrap_pad_styled_user_echo_block(block, wrap_cols_)) {
        span_append_->append_line(row);
    }
}

void PaneScrollView::ProseSegment::append(const std::vector<StyledLine>& lines) {
    if (!span_append_) return;
    std::size_t i = 0;
    while (i < lines.size()) {
        if (!arbiter::is_styled_user_echo_line(lines[i])) {
            source_.push_back(lines[i]);
            emit_line(lines[i]);
            ++i;
            continue;
        }
        const std::size_t src_start = source_.size();
        while (i < lines.size() && arbiter::is_styled_user_echo_line(lines[i])) {
            source_.push_back(lines[i]);
            ++i;
        }
        emit_echo_run(source_.data() + src_start, source_.data() + source_.size());
    }
}

void PaneScrollView::ProseSegment::clear() {
    source_.clear();
    if (span_append_) span_append_->clear();
}

void PaneScrollView::ProseSegment::retheme() {
    if (!span_append_) return;
    span_append_->clear();
    std::size_t i = 0;
    while (i < source_.size()) {
        if (!arbiter::is_styled_user_echo_line(source_[i])) {
            emit_line(source_[i]);
            ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < source_.size() && arbiter::is_styled_user_echo_line(source_[i])) {
            ++i;
        }
        emit_echo_run(source_.data() + start, source_.data() + i);
    }
}

bool PaneScrollView::ProseSegment::is_empty() const {
    return buffer_ == 0 || textBufferGetLength(buffer_) == 0;
}

int PaneScrollView::ProseSegment::visual_rows(int content_w) const {
    if (view_ == 0 || is_empty()) return 0;
    const int next = std::max(1, content_w);
    // Keep emit pad / HR width in sync — do not mutate wrap_cols_ alone.
    if (next != wrap_cols_) {
        const_cast<ProseSegment*>(this)->set_wrap_cols(next);
    } else if (view_ != 0) {
        textBufferViewSetWrapWidth(view_, static_cast<std::uint32_t>(next));
    }
    return static_cast<int>(textBufferViewGetVirtualLineCount(view_));
}

void PaneScrollView::ProseSegment::set_wrap_cols(int cols) {
    const int next = std::max(1, cols);
    const bool rules_resized = arbiter::resize_styled_rule_lines(source_, next);
    bool has_echo = false;
    for (const StyledLine& line : source_) {
        if (arbiter::is_styled_user_echo_line(line)) {
            has_echo = true;
            break;
        }
    }
    const bool wrap_changed = next != wrap_cols_;
    wrap_cols_ = next;
    if (view_ != 0) {
        textBufferViewSetWrapWidth(view_, static_cast<std::uint32_t>(wrap_cols_));
    }
    // Rules mutate source_; user-echo pad is emit-only — rebuild buffer when
    // wrap width changes so the bg strip tracks the content row.
    if (rules_resized || (has_echo && wrap_changed)) retheme();
}

void PaneScrollView::ProseSegment::collect_lines(std::vector<std::string>& out) const {
    // Always emit every source line so find_rows index k maps to visual row k.
    for (const auto& line : source_) out.push_back(line.text);
}

bool PaneScrollView::ProseSegment::find_skip_line(std::size_t index) const {
    if (index >= source_.size()) return false;
    // Skip echoed `/find` chrome only — not model prose that mentions /find.
    return arbiter::is_user_echo_find_command(source_[index]);
}

void PaneScrollView::ProseSegment::draw(OpenTuiHandle frame,
                                        int x,
                                        int y,
                                        int w,
                                        int h,
                                        int skip_rows) const {
    // Negative origins cast to uint32_t wrap to ~0 and SIGSEGV inside
    // OpenTUI's trySetTransparentTextCellFast (repro: x=-1, y=0).
    if (view_ == 0 || w <= 0 || h <= 0 || x < 0 || y < 0) return;
    textBufferViewSetFirstLineOffset(view_, 0);
    textBufferViewSetViewport(view_,
                              0,
                              static_cast<std::uint32_t>(std::max(0, skip_rows)),
                              static_cast<std::uint32_t>(w),
                              static_cast<std::uint32_t>(h));
    bufferDrawTextBufferView(frame, view_,
                             static_cast<std::uint32_t>(x),
                             static_cast<std::uint32_t>(y));
}

// --- TextSegment (legacy ANSI scrollback) -------------------------------------

PaneScrollView::TextSegment::TextSegment() {
    buffer_ = createTextBuffer(/*width_method wcwidth=*/0);
    if (buffer_ == 0) throw std::runtime_error("createTextBuffer failed");
    view_ = createTextBufferView(buffer_);
    if (view_ == 0) {
        destroyTextBuffer(buffer_);
        buffer_ = 0;
        throw std::runtime_error("createTextBufferView failed");
    }
    textBufferViewSetWrapMode(view_, kWrapWord);
    textBufferViewSetFirstLineOffset(view_, 0);
    styled_append_ = std::make_unique<AnsiScrollAppender>(buffer_);
}

PaneScrollView::TextSegment::~TextSegment() {
    if (view_ != 0) destroyTextBufferView(view_);
    if (buffer_ != 0) destroyTextBuffer(buffer_);
}

void PaneScrollView::TextSegment::append(std::string_view text) {
    if (text.empty() || !styled_append_) return;
    source_.append(text.data(), text.size());
    styled_append_->append(text);
}

void PaneScrollView::TextSegment::clear() {
    source_.clear();
    if (styled_append_) styled_append_->clear();
}

void PaneScrollView::TextSegment::retheme() {
    if (!styled_append_ || source_.empty()) return;
    styled_append_->clear();
    styled_append_->append(source_);
}

bool PaneScrollView::TextSegment::is_empty() const {
    return buffer_ == 0 || textBufferGetLength(buffer_) == 0;
}

int PaneScrollView::TextSegment::visual_rows(int content_w) const {
    if (view_ == 0 || is_empty()) return 0;
    const_cast<TextSegment*>(this)->wrap_cols_ = content_w;
    textBufferViewSetWrapWidth(view_, static_cast<std::uint32_t>(content_w));
    return static_cast<int>(textBufferViewGetVirtualLineCount(view_));
}

void PaneScrollView::TextSegment::set_wrap_cols(int cols) {
    wrap_cols_ = std::max(1, cols);
    if (view_ != 0) {
        textBufferViewSetWrapWidth(view_, static_cast<std::uint32_t>(wrap_cols_));
    }
}

void PaneScrollView::TextSegment::collect_lines(std::vector<std::string>& out) const {
    const std::string plain = arbiter::strip_ansi(source_);
    size_t start = 0;
    while (start <= plain.size()) {
        const size_t nl = plain.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(plain.substr(start));
            break;
        }
        out.push_back(plain.substr(start, nl - start));
        start = nl + 1;
    }
}

void PaneScrollView::TextSegment::draw(OpenTuiHandle frame,
                                       int x,
                                       int y,
                                       int w,
                                       int h,
                                       int skip_rows) const {
    if (view_ == 0 || w <= 0 || h <= 0 || x < 0 || y < 0) return;
    textBufferViewSetFirstLineOffset(view_, 0);
    textBufferViewSetViewport(view_,
                              0,
                              static_cast<std::uint32_t>(std::max(0, skip_rows)),
                              static_cast<std::uint32_t>(w),
                              static_cast<std::uint32_t>(h));
    bufferDrawTextBufferView(frame, view_,
                             static_cast<std::uint32_t>(x),
                             static_cast<std::uint32_t>(y));
}

// --- CodeSegment (panel-style fenced code blocks) ------------------------------

void PaneScrollView::CodeSegment::open(std::string lang, size_t preview_rows) {
    lang_ = normalize_code_lang(lang);
    preview_rows_ = preview_rows;
    lines_.clear();
    highlighted_.clear();
    close_fence_.clear();
    closed_ = false;
    expanded_ = false;
    cached_rows_ = -1;
}

void PaneScrollView::CodeSegment::append_line(std::string line) {
    lines_.push_back(std::move(line));
    highlighted_.push_back(highlight_code_line(lang_, lines_.back()));
    cached_rows_ = -1;
}

void PaneScrollView::CodeSegment::close(std::string close_fence) {
    close_fence_ = std::move(close_fence);
    closed_ = true;
    highlighted_ = highlight_code_block(lang_, lines_);
    cached_rows_ = -1;
}

size_t PaneScrollView::CodeSegment::visible_body_count() const {
    if (expanded_ || preview_rows_ == 0) return lines_.size();
    return std::min(lines_.size(), preview_rows_);
}

void PaneScrollView::CodeSegment::toggle_expanded() {
    if (!is_truncated() && !expanded_) return;
    expanded_ = !expanded_;
    cached_rows_ = -1;
}

void PaneScrollView::CodeSegment::rehighlight() {
    highlighted_.clear();
    for (const auto& line : lines_) {
        highlighted_.push_back(highlight_code_line(lang_, line));
    }
    cached_rows_ = -1;
}

int PaneScrollView::CodeSegment::gutter_width() const {
    const std::uint32_t max_line = static_cast<std::uint32_t>(
        std::max<std::size_t>(lines_.size(), 1));
    std::uint32_t digits = 1;
    std::uint32_t n = max_line;
    while (n >= 10) {
        n /= 10;
        ++digits;
    }
    return static_cast<int>(digits) + 2;
}

int PaneScrollView::CodeSegment::visual_rows(int /*content_w*/) const {
    if (cached_rows_ >= 0) return cached_rows_;
    int rows = 1;
    rows += static_cast<int>(visible_body_count());
    if (!expanded_ && preview_rows_ > 0 && lines_.size() > preview_rows_) rows += 1;
    cached_rows_ = rows;
    return rows;
}

void PaneScrollView::CodeSegment::set_wrap_cols(int /*cols*/) {
    cached_rows_ = -1;
}

void PaneScrollView::CodeSegment::collect_lines(std::vector<std::string>& out) const {
    out.push_back(lang_);   // header row
    // All body lines are searchable even when the block is collapsed —
    // find_rows clamps hits beyond the preview back into the visible rows.
    for (const auto& line : lines_) out.push_back(line);
}

void PaneScrollView::CodeSegment::draw(OpenTuiHandle frame,
                                       int x,
                                       int y,
                                       int w,
                                       int h,
                                       int skip_rows) const {
    if (w <= 0 || h <= 0) return;

    const TuiDesign& d = tui_design();
    const int gutter = gutter_width();
    const int content_x = x + gutter;
    const int content_w = std::max(1, w - gutter);

    auto draw_plain_row = [&](const std::string& text,
                              const TuiRgba& fg,
                              const TuiRgba& bg,
                              int& screen_row) -> bool {
        if (screen_row < skip_rows) {
            ++screen_row;
            return true;
        }
        const int drawn = screen_row - skip_rows;
        if (drawn >= h) return false;

        fill_rect(frame, x, y + drawn, w, 1, bg);
        draw_text(frame, content_x, y + drawn, trim_to_cells(text, content_w), fg, bg);
        ++screen_row;
        return true;
    };

    auto draw_styled_row = [&](const StyledLine& line,
                               std::uint32_t line_num,
                               const TuiRgba& bg,
                               int& screen_row) -> bool {
        if (screen_row < skip_rows) {
            ++screen_row;
            return true;
        }
        const int drawn = screen_row - skip_rows;
        if (drawn >= h) return false;

        fill_rect(frame, x, y + drawn, w, 1, bg);

        char buf[24];
        std::snprintf(buf, sizeof(buf), "%*u ",
                      gutter - 2,
                      line_num);
        draw_text(frame, x + 1, y + drawn, buf, d.content.code_gutter, bg);

        int col = 0;
        const auto fg_for = [&](StyleId id) -> const TuiRgba& {
            if (id == StyleId::Code) return d.text.primary;
            const ResolvedStyle rs = resolve_style(id);
            return rs.fg ? *rs.fg : d.text.primary;
        };
        const auto attrs_for = [&](StyleId id) -> std::uint32_t {
            const ResolvedStyle rs = resolve_style(id);
            return rs.attrs;
        };

        const auto emit_span = [&](std::size_t begin, std::size_t end, StyleId id) {
            if (begin >= line.text.size() || end <= begin) return;
            const std::size_t end_clamped = std::min(end, line.text.size());
            const std::string chunk = trim_to_cells(line.text.substr(begin, end_clamped - begin),
                                                    content_w - col);
            if (chunk.empty()) return;
            draw_text(frame,
                      content_x + col,
                      y + drawn,
                      chunk,
                      fg_for(id),
                      bg,
                      attrs_for(id));
            col += cell_width(chunk);
        };

        if (line.spans.empty()) {
            emit_span(0, line.text.size(), StyleId::Code);
        } else {
            std::size_t cursor = 0;
            for (const StyleSpan& span : line.spans) {
                if (span.begin > line.text.size()) break;
                if (span.begin > cursor) {
                    emit_span(cursor, span.begin, StyleId::Code);
                }
                const std::size_t end = std::min(static_cast<std::size_t>(span.end), line.text.size());
                if (end > span.begin) emit_span(span.begin, end, span.id);
                cursor = end;
            }
            if (cursor < line.text.size()) emit_span(cursor, line.text.size(), StyleId::Code);
        }

        ++screen_row;
        return true;
    };

    int row = 0;

    if (row >= skip_rows) {
        const int drawn = row - skip_rows;
        if (drawn < h) {
            fill_rect(frame, x, y + drawn, w, 1, d.content.code_header_bg);
            const std::string title = lang_.empty() ? "code" : lang_;
            draw_text(frame,
                      x + 1,
                      y + drawn,
                      title,
                      d.text.primary,
                      d.content.code_header_bg);
            // Left accent bar so code panels share chrome language with input.
            if (w > 0) {
                fill_rect(frame, x, y + drawn, 1, 1, d.border.subtle);
            }
        }
    }
    ++row;
    if (row - skip_rows >= h) return;

    const size_t show = visible_body_count();
    for (size_t i = 0; i < show; ++i) {
        if (i >= highlighted_.size()) continue;
        if (!draw_styled_row(highlighted_[i],
                             static_cast<std::uint32_t>(i + 1),
                             d.content.code_bg,
                             row)) {
            return;
        }
    }

    if (!expanded_ && preview_rows_ > 0 && lines_.size() > preview_rows_) {
        const std::string summary =
            "… (" + std::to_string(lines_.size()) + " lines, ^O expand) …";
        if (!draw_plain_row(summary, d.content.code_gutter, d.content.code_bg, row)) return;
    }
}

// --- DiffSegment (rich split diff panel) ------------------------------------

PaneScrollView::DiffSegment::DiffSegment(std::string patch)
    : patch_(std::move(patch)) {}

int PaneScrollView::DiffSegment::visual_rows(int /*content_w*/) const {
    if (!cached_) {
        panel_.set_patch(patch_);
        cached_rows_ = panel_.visual_rows();
        cached_ = true;
    }
    return cached_rows_;
}

void PaneScrollView::DiffSegment::set_wrap_cols(int /*cols*/) {
    cached_ = false;
}

void PaneScrollView::DiffSegment::collect_lines(std::vector<std::string>& out) const {
    size_t start = 0;
    while (start <= patch_.size()) {
        const size_t nl = patch_.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(patch_.substr(start));
            break;
        }
        out.push_back(patch_.substr(start, nl - start));
        start = nl + 1;
    }
}

void PaneScrollView::DiffSegment::draw(OpenTuiHandle frame,
                                      int x,
                                      int y,
                                      int w,
                                      int h,
                                      int skip_rows) const {
    if (patch_.empty() || w <= 0 || h <= 0) return;
    if (!cached_) {
        panel_.set_patch(patch_);
        cached_ = true;
    }
    panel_.draw(frame, x, y, w, h, skip_rows);
}

// --- ThinkingSegment (provider reasoning, collapsed by default) --------------

namespace {

// Soft-wrap a styled line to `cols`, remapping spans onto each visual row.
std::vector<StyledLine> wrap_styled_line(const StyledLine& line, int cols) {
    if (cols < 1) cols = 1;
    if (line.text.empty()) return {line};
    if (static_cast<int>(arbiter::display_width(line.text)) <= cols) return {line};

    std::vector<StyledLine> out;
    std::string remain = line.text;
    auto style_at = [&](std::uint32_t byte_off) -> StyleId {
        for (const StyleSpan& span : line.spans) {
            if (byte_off >= span.begin && byte_off < span.end) return span.id;
        }
        return StyleId::Default;
    };

    std::uint32_t origin = 0;
    while (!remain.empty()) {
        std::string chunk = arbiter::trim_to_display_cols(remain, cols);
        if (chunk.empty()) chunk.assign(remain, 0, 1);

        StyledLine row;
        std::size_t i = 0;
        while (i < chunk.size()) {
            const StyleId id = style_at(origin + static_cast<std::uint32_t>(i));
            std::size_t j = i + 1;
            while (j < chunk.size() &&
                   style_at(origin + static_cast<std::uint32_t>(j)) == id) {
                ++j;
            }
            styled_append(row, id, std::string_view(chunk).substr(i, j - i));
            i = j;
        }
        if (row.spans.empty() && chunk.empty()) {
            row.spans.push_back({0, 0, StyleId::Dim});
        }
        out.push_back(std::move(row));
        origin += static_cast<std::uint32_t>(chunk.size());
        remain.erase(0, chunk.size());
    }
    return out;
}

std::vector<StyledLine> render_thinking_markdown(const std::string& text) {
    MarkdownRenderer md;
    std::vector<StyledLine> lines = md.feed_styled(text);
    std::vector<StyledLine> tail = md.flush_styled();
    lines.insert(lines.end(),
                 std::make_move_iterator(tail.begin()),
                 std::make_move_iterator(tail.end()));
    while (!lines.empty() && lines.back().text.empty()) lines.pop_back();
    return lines;
}

constexpr std::uint32_t kThinkingDimAttr = 1u << 1;  // matches style_resolver Dim
constexpr std::string_view kTruncationMark = "\u2026";

const TuiRgba& thinking_fg(StyleId id, const TuiDesign& d) {
    // Prefer muted/readable defaults; keep markdown accent hues for structure.
    switch (id) {
    case StyleId::Default:
    case StyleId::Bold:
    case StyleId::Italic:
        return d.text.muted;
    case StyleId::Dim:
    case StyleId::Strike:
    case StyleId::Blockquote:
    case StyleId::Rule:
    case StyleId::System:
        return d.content.text_dim;
    default:
        break;
    }
    const ResolvedStyle rs = resolve_style(id);
    return rs.fg ? *rs.fg : d.text.muted;
}

std::uint32_t thinking_attrs(StyleId id) {
    const ResolvedStyle rs = resolve_style(id);
    return rs.attrs | kThinkingDimAttr;
}

// Fold the truncation mark onto the last visible body row so it does not
// sit alone against the bottom border.
StyledLine with_truncation_mark(const StyledLine& line, int body_cols) {
    const int mark_w = static_cast<int>(arbiter::display_width(kTruncationMark));
    const int used = static_cast<int>(arbiter::display_width(line.text));
    if (used + mark_w <= body_cols) {
        StyledLine out = line;
        styled_append(out, StyleId::Dim, kTruncationMark);
        return out;
    }

    const int keep = std::max(0, body_cols - mark_w);
    auto parts = wrap_styled_line(line, std::max(1, keep));
    StyledLine out = parts.empty() ? StyledLine{} : std::move(parts.front());
    // Ensure the kept prefix never exceeds `keep` cells (wrap may still
    // produce a full-width first piece when keep==0).
    if (static_cast<int>(arbiter::display_width(out.text)) > keep) {
        const std::string trimmed =
            arbiter::trim_to_display_cols(out.text, keep);
        StyledLine rebuilt;
        std::size_t cursor = 0;
        for (const StyleSpan& span : out.spans) {
            if (span.begin >= trimmed.size()) break;
            if (span.begin > cursor) {
                styled_append(rebuilt, StyleId::Default,
                              std::string_view(trimmed).substr(
                                  cursor, span.begin - cursor));
            }
            const std::size_t end =
                std::min(static_cast<std::size_t>(span.end), trimmed.size());
            if (end > span.begin) {
                styled_append(rebuilt, span.id,
                              std::string_view(trimmed).substr(
                                  span.begin, end - span.begin));
            }
            cursor = end;
        }
        if (cursor < trimmed.size()) {
            styled_append(rebuilt, StyleId::Default,
                          std::string_view(trimmed).substr(cursor));
        }
        out = std::move(rebuilt);
    }
    styled_append(out, StyleId::Dim, kTruncationMark);
    return out;
}

} // namespace

void PaneScrollView::ThinkingSegment::invalidate_cache() const {
    cache_src_.clear();
    cache_cols_ = -1;
    body_cache_.clear();
}

void PaneScrollView::ThinkingSegment::append(std::string_view delta) {
    text_.append(delta.data(), delta.size());
    invalidate_cache();
}

void PaneScrollView::ThinkingSegment::set_agent_id(std::string_view agent_id) {
    if (agent_id.empty()) return;
    if (agent_id_.empty()) agent_id_.assign(agent_id.data(), agent_id.size());
}

void PaneScrollView::ThinkingSegment::toggle_expanded() {
    if (!can_expand()) return;
    expanded_ = !expanded_;
}

int PaneScrollView::ThinkingSegment::body_content_cols(int content_w) const {
    const int cols = std::max(1, content_w > 0 ? content_w : wrap_cols_);
    return std::max(1, cols - kBoxChromeCols);
}

const std::vector<StyledLine>&
PaneScrollView::ThinkingSegment::wrapped_body(int body_cols) const {
    if (cache_cols_ == body_cols && cache_src_ == text_) return body_cache_;
    cache_src_ = text_;
    cache_cols_ = body_cols;
    body_cache_.clear();
    if (text_.empty()) return body_cache_;

    const auto rendered = render_thinking_markdown(text_);
    body_cache_.reserve(rendered.size());
    for (const StyledLine& line : rendered) {
        auto parts = wrap_styled_line(line, body_cols);
        body_cache_.insert(body_cache_.end(),
                           std::make_move_iterator(parts.begin()),
                           std::make_move_iterator(parts.end()));
    }
    return body_cache_;
}

bool PaneScrollView::ThinkingSegment::can_expand() const {
    const int body_n =
        static_cast<int>(wrapped_body(body_content_cols(wrap_cols_)).size());
    if (expanded_) return body_n > 0;
    return body_n > kPreviewRows;
}

std::string PaneScrollView::ThinkingSegment::header_text() const {
    return "thinking";
}

int PaneScrollView::ThinkingSegment::visual_rows(int content_w) const {
    const auto& body = wrapped_body(body_content_cols(content_w));
    const int body_n = static_cast<int>(body.size());
    // Top border (inline title) + bottom border. Truncation mark folds into
    // the last visible body row — no dedicated ellipsis row.
    static constexpr int kChromeRows = 2;
    if (!expanded_) {
        if (body_n == 0) return kChromeRows;
        return kChromeRows + std::min(body_n, kPreviewRows);
    }
    return kChromeRows + std::min(body_n, kExpandedCap);
}

void PaneScrollView::ThinkingSegment::set_wrap_cols(int cols) {
    wrap_cols_ = std::max(1, cols);
    invalidate_cache();
}

void PaneScrollView::ThinkingSegment::collect_lines(std::vector<std::string>& out) const {
    // Line k maps to visual row k: title-bearing top border is row 0, body
    // follows, bottom border contributes a final blank.
    out.push_back(header_text());
    if (text_.empty()) {
        out.emplace_back();
        return;
    }
    const int body_cols = body_content_cols(wrap_cols_);
    const auto& body = wrapped_body(body_cols);
    const int body_n = static_cast<int>(body.size());
    const int limit = expanded_
        ? std::min(body_n, kExpandedCap)
        : std::min(body_n, kPreviewRows);
    const bool truncated = expanded_ ? body_n > kExpandedCap
                                     : body_n > kPreviewRows;
    for (int i = 0; i < limit; ++i) {
        if (truncated && i == limit - 1) {
            out.push_back(with_truncation_mark(body[static_cast<size_t>(i)],
                                               body_cols).text);
        } else {
            out.push_back(body[static_cast<size_t>(i)].text);
        }
    }
    out.emplace_back();  // bottom border
}

void PaneScrollView::ThinkingSegment::draw(OpenTuiHandle frame,
                                           int x,
                                           int y,
                                           int w,
                                           int h,
                                           int skip_rows) const {
    if (w < 2 || h <= 0) return;
    const TuiDesign& d = tui_design();
    const TuiRgba& bg = d.bg.scroll;
    const TuiRgba& border_fg = d.text.muted;
    const int body_cols = std::max(1, w - kBoxChromeCols);
    const int text_x = x + 2;

    // Row-clipped painter: clears the full row width before painting so
    // streaming growth never leaves stale border cells behind.
    int screen_row = 0;
    auto emit = [&](auto&& painter) -> bool {
        if (screen_row < skip_rows) {
            ++screen_row;
            return true;
        }
        const int drawn = screen_row - skip_rows;
        if (drawn >= h) return false;
        fill_rect(frame, x, y + drawn, w, 1, bg);
        painter(y + drawn);
        ++screen_row;
        return true;
    };

    // Top border — plain "thinking" breaks the horizontal run.
    if (!emit([&](int yy) {
            draw_rounded_box_row(frame, x, yy, w, /*top=*/true, border_fg, bg,
                                 header_text(), &d.accent.info);
        })) {
        return;
    }

    auto draw_styled_body = [&](const StyledLine& line) -> bool {
        return emit([&](int yy) {
            draw_text(frame, x, yy, d.border.vertical, border_fg, bg);
            draw_text(frame, x + w - 1, yy, d.border.vertical, border_fg, bg);

            int col = 0;
            const auto emit_span = [&](std::size_t begin, std::size_t end,
                                       StyleId id) {
                if (begin >= line.text.size() || end <= begin) return;
                const std::size_t end_clamped =
                    std::min(end, line.text.size());
                const std::string chunk = trim_to_cells(
                    line.text.substr(begin, end_clamped - begin),
                    body_cols - col);
                if (chunk.empty()) return;
                draw_text(frame,
                          text_x + col,
                          yy,
                          chunk,
                          thinking_fg(id, d),
                          bg,
                          thinking_attrs(id));
                col += cell_width(chunk);
            };

            if (line.spans.empty()) {
                emit_span(0, line.text.size(), StyleId::Default);
            } else {
                std::size_t cursor = 0;
                for (const StyleSpan& span : line.spans) {
                    if (span.begin > line.text.size()) break;
                    if (span.begin > cursor) {
                        emit_span(cursor, span.begin, StyleId::Default);
                    }
                    const std::size_t end = std::min(
                        static_cast<std::size_t>(span.end), line.text.size());
                    if (end > span.begin) emit_span(span.begin, end, span.id);
                    cursor = end;
                }
                if (cursor < line.text.size()) {
                    emit_span(cursor, line.text.size(), StyleId::Default);
                }
            }
        });
    };

    if (!text_.empty()) {
        const auto& body = wrapped_body(body_cols);
        const int body_n = static_cast<int>(body.size());
        const int limit = expanded_
            ? std::min(body_n, kExpandedCap)
            : std::min(body_n, kPreviewRows);
        const bool truncated = expanded_ ? body_n > kExpandedCap
                                         : body_n > kPreviewRows;
        for (int i = 0; i < limit; ++i) {
            StyledLine row = body[static_cast<size_t>(i)];
            if (truncated && i == limit - 1) {
                row = with_truncation_mark(row, body_cols);
            }
            if (!draw_styled_body(row)) return;
        }
    }

    emit([&](int yy) {
        draw_rounded_box_row(frame, x, yy, w, /*top=*/false, border_fg, bg);
    });
}

// --- ToolSegment (per-tool activity timeline) ---------------------------------

void PaneScrollView::ToolSegment::apply(const ToolActivityEvent& event) {
    if (!event.id.empty()) id_ = event.id;
    if (!event.label.empty()) label_ = event.label;
    if (!event.kind.empty()) kind_ = event.kind;
    if (!event.detail.empty()) detail_ = event.detail;
    if (event.phase == ToolActivityEvent::Phase::Finished) {
        finished_ = true;
        ok_ = event.ok;
        if (!event.result_preview.empty()) {
            result_preview_ = event.result_preview;
        }
    } else {
        finished_ = false;
        ok_ = true;
    }
}

void PaneScrollView::ToolSegment::toggle_expanded() {
    if (!can_expand()) return;
    expanded_ = !expanded_;
}

bool PaneScrollView::ToolSegment::can_expand() const {
    return !detail_.empty() || !result_preview_.empty();
}

std::string PaneScrollView::ToolSegment::status_glyph() const {
    if (!finished_) return "\u25CB";  // ○ running
    return ok_ ? "\u2713" : "\u2717"; // ✓ / ✗
}

std::string PaneScrollView::ToolSegment::header_text() const {
    std::string text = status_glyph();
    text += ' ';
    text += label_.empty() ? (kind_.empty() ? "tool" : kind_) : label_;
    if (can_expand()) {
        text += expanded_ ? "  \u25BE" : "  \u25B8"; // ▾ / ▸
    }
    return text;
}

int PaneScrollView::ToolSegment::visual_rows(int /*content_w*/) const {
    if (!expanded_ || !can_expand()) return 1;
    int rows = 1;
    if (!detail_.empty()) ++rows;
    if (!result_preview_.empty()) ++rows;
    return rows;
}

void PaneScrollView::ToolSegment::set_wrap_cols(int cols) {
    wrap_cols_ = std::max(1, cols);
}

void PaneScrollView::ToolSegment::collect_lines(std::vector<std::string>& out) const {
    out.push_back(header_text());
    if (expanded_) {
        if (!detail_.empty()) out.push_back("  " + detail_);
        if (!result_preview_.empty()) out.push_back("  " + result_preview_);
    }
}

void PaneScrollView::ToolSegment::draw(OpenTuiHandle frame,
                                       int x,
                                       int y,
                                       int w,
                                       int h,
                                       int skip_rows) const {
    if (w <= 0 || h <= 0) return;
    const TuiDesign& d = tui_design();
    const TuiRgba& bg = d.bg.scroll;

    auto draw_row = [&](const std::string& text,
                        const TuiRgba& fg,
                        std::uint32_t attrs,
                        int& screen_row) -> bool {
        if (screen_row < skip_rows) {
            ++screen_row;
            return true;
        }
        const int drawn = screen_row - skip_rows;
        if (drawn >= h) return false;
        fill_rect(frame, x, y + drawn, w, 1, bg);
        // Quiet left accent so tool rows don't read as model prose.
        if (w > 0) {
            fill_rect(frame, x, y + drawn, 1, 1, d.border.subtle);
        }
        draw_text(frame,
                  x + 2,
                  y + drawn,
                  trim_to_cells(text, std::max(1, w - 3)),
                  fg,
                  bg,
                  attrs);
        ++screen_row;
        return true;
    };

    int row = 0;
    const TuiRgba* header_fg = &d.content.system_fg;
    std::uint32_t header_attrs = 0;
    if (!finished_) {
        header_fg = &d.accent.info;
    } else if (ok_) {
        header_fg = &d.accent.success;
    } else {
        header_fg = &d.accent.error;
    }
    if (!draw_row(header_text(), *header_fg, header_attrs, row)) return;

    if (expanded_) {
        if (!detail_.empty()) {
            if (!draw_row(detail_, d.text.muted, 0, row)) return;
        }
        if (!result_preview_.empty()) {
            if (!draw_row(result_preview_, d.content.system_fg, 0, row)) return;
        }
    }
}

#ifdef ARBITER_HAS_NATIVE_DIFF_VIEW

PaneScrollView::NativeDiffSegment::NativeDiffSegment(std::string patch)
    : patch_(std::move(patch)) {}

int PaneScrollView::NativeDiffSegment::visual_rows(int content_w) const {
    if (patch_.empty()) return 0;
    if (cached_width_ != content_w || cached_rows_ <= 0) {
        diff_.set_patch(patch_);
        diff_.set_wrap_mode(kWrapWord);
        diff_.set_wrap_width(static_cast<std::uint32_t>(std::max(1, content_w)));
        cached_rows_ = static_cast<int>(diff_.virtual_line_count());
        cached_width_ = content_w;
        if (cached_rows_ <= 0) cached_rows_ = 1;
    }
    return cached_rows_;
}

void PaneScrollView::NativeDiffSegment::set_wrap_cols(int cols) {
    cached_width_ = -1;
    diff_.set_wrap_width(static_cast<std::uint32_t>(std::max(1, cols)));
}

void PaneScrollView::NativeDiffSegment::draw(OpenTuiHandle frame,
                                      int x,
                                      int y,
                                      int w,
                                      int h,
                                      int skip_rows) const {
    if (patch_.empty() || w <= 0 || h <= 0 || x < 0 || y < 0 || !diff_.valid()) return;
    diff_.set_scroll_y(static_cast<std::uint32_t>(std::max(0, skip_rows)));
    diff_.draw(frame, x, y, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
}

#endif

// --- PaneScrollView -----------------------------------------------------------

PaneScrollView::PaneScrollView() {
    segments_.push_back(std::make_unique<ProseSegment>());
}

PaneScrollView::~PaneScrollView() = default;

PaneScrollView::TextSegment& PaneScrollView::current_text() {
    if (!segments_.empty() && segments_.back()->is_text()) {
        return static_cast<TextSegment&>(*segments_.back());
    }
    auto seg = std::make_unique<TextSegment>();
    auto& ref = *seg;
    segments_.push_back(std::move(seg));
    return ref;
}

PaneScrollView::ProseSegment& PaneScrollView::current_prose() {
    if (!segments_.empty() && segments_.back()->is_prose()) {
        return static_cast<ProseSegment&>(*segments_.back());
    }
    auto seg = std::make_unique<ProseSegment>();
    auto& ref = *seg;
    segments_.push_back(std::move(seg));
    return ref;
}

PaneScrollView::CodeSegment& PaneScrollView::current_code() {
    if (!segments_.empty()) {
        if (auto* code = dynamic_cast<CodeSegment*>(segments_.back().get())) {
            if (!code->closed_) return *code;
        }
    }
    // Callers (append_code_open) own block/panel gap insertion.
    auto seg = std::make_unique<CodeSegment>();
    auto& ref = *seg;
    segments_.push_back(std::move(seg));
    return ref;
}

void PaneScrollView::bind(const TUI& tui) {
    const TuiDesign& d = tui_design();
    const int pad = tui_pane_edge_pad(tui.cols(), d);
    const int gutter = std::max(0, std::min(d.layout.scroll_gutter_cols,
                                            std::max(0, tui.cols() - pad * 2 - 1)));
    const int pad_y = std::max(0, d.layout.scroll_pad_y);
    const int content_w = std::max(1, tui.cols() - (pad * 2) - gutter);
    const int region_h = tui.scroll_region_rows();
    const int content_h = std::max(1, region_h - pad_y * 2);

    buf_x_ = tui.left_col() - 1 + pad + gutter;
    buf_y_ = tui.scroll_top_row() - 1 + pad_y;
    viewport_w_ = content_w;
    viewport_h_ = content_h;
    set_wrap_cols(content_w);
}

void PaneScrollView::set_wrap_cols(int cols) {
    wrap_cols_ = std::max(1, cols);
    for (auto& seg : segments_) seg->set_wrap_cols(wrap_cols_);
}

bool PaneScrollView::has_rendered_content() const {
    for (const auto& seg : segments_) {
        if (const auto* prose = dynamic_cast<const ProseSegment*>(seg.get())) {
            if (!prose->is_empty()) return true;
        } else if (const auto* text = dynamic_cast<const TextSegment*>(seg.get())) {
            if (!text->is_empty()) return true;
        } else if (const auto* code = dynamic_cast<const CodeSegment*>(seg.get())) {
            if (code->has_content()) return true;
        } else if (!dynamic_cast<const BlankSegment*>(seg.get())) {
            return true;
        }
    }
    return false;
}

PaneScrollView::SegmentKind PaneScrollView::last_content_kind() const {
    for (auto it = segments_.rbegin(); it != segments_.rend(); ++it) {
        if (dynamic_cast<const BlankSegment*>(it->get())) continue;
        if (dynamic_cast<const HistoryGapSegment*>(it->get())) continue;
        if (const auto* prose = dynamic_cast<const ProseSegment*>(it->get())) {
            if (prose->is_empty()) continue;
            return SegmentKind::Prose;
        }
        if (const auto* text = dynamic_cast<const TextSegment*>(it->get())) {
            if (text->is_empty()) continue;
            return SegmentKind::Text;
        }
        if (dynamic_cast<const CodeSegment*>(it->get())) return SegmentKind::Code;
        if (dynamic_cast<const DiffSegment*>(it->get())) return SegmentKind::Diff;
#ifdef ARBITER_HAS_NATIVE_DIFF_VIEW
        if (dynamic_cast<const NativeDiffSegment*>(it->get())) return SegmentKind::Diff;
#endif
        if (dynamic_cast<const ToolSegment*>(it->get())) return SegmentKind::Tool;
        if (dynamic_cast<const ThinkingSegment*>(it->get())) return SegmentKind::Thinking;
        return SegmentKind::Other;
    }
    return SegmentKind::None;
}

void PaneScrollView::trim_trailing_soft_blanks() {
    // Walk back over BlankSegments to reach the last content segment, then
    // drop trailing empty prose lines that are NOT user-echo pad rows / text
    // newlines so BlankSegment gaps stay a single consistent rhythm.
    Segment* content = nullptr;
    for (auto it = segments_.rbegin(); it != segments_.rend(); ++it) {
        if (dynamic_cast<BlankSegment*>(it->get())) continue;
        content = it->get();
        break;
    }
    if (!content) return;

    if (auto* prose = dynamic_cast<ProseSegment*>(content)) {
        bool changed = false;
        while (!prose->source_.empty()) {
            const StyledLine& last = prose->source_.back();
            if (!last.text.empty()) break;
            // Keep blank user-echo pad rows — they are intentional chrome and
            // also count toward the inter-block gap (see ensure_block_gap).
            if (arbiter::is_styled_user_echo_line(last)) break;
            prose->source_.pop_back();
            changed = true;
        }
        if (changed) prose->retheme();
        return;
    }
    if (auto* text = dynamic_cast<TextSegment*>(content)) {
        if (text->source_.empty()) return;
        std::string& s = text->source_;
        bool changed = false;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
            s.pop_back();
            changed = true;
        }
        if (changed) text->retheme();
    }
}

int PaneScrollView::trailing_separator_rows() const {
    int n = 0;
    for (auto it = segments_.rbegin(); it != segments_.rend(); ++it) {
        if (dynamic_cast<const BlankSegment*>(it->get())) {
            ++n;
            continue;
        }
        if (const auto* prose = dynamic_cast<const ProseSegment*>(it->get())) {
            for (auto lit = prose->source_.rbegin(); lit != prose->source_.rend(); ++lit) {
                if (!lit->text.empty()) break;
                ++n;  // empty prose / user-echo pad rows
            }
        } else if (const auto* text = dynamic_cast<const TextSegment*>(it->get())) {
            // Source should already be trimmed of trailing newlines; a lone
            // trailing `\n` still counts as one separator row if present.
            if (!text->source_.empty()
                && (text->source_.back() == '\n' || text->source_.back() == '\r')) {
                ++n;
            }
        }
        break;
    }
    return n;
}

void PaneScrollView::ensure_block_gap(SegmentKind next, int gap_rows, bool force) {
    if (gap_rows <= 0) return;
    // First content in an empty pane: leave one blank under the header chrome
    // so the lead block (usually a user echo) doesn't kiss the top edge.
    if (!has_rendered_content()) {
        (void)next;
        (void)force;
        for (int i = 0; i < gap_rows; ++i) append_blank_row();
        return;
    }
    const SegmentKind prev = last_content_kind();
    if (prev == SegmentKind::None) return;
    // Consecutive tool rows form one timeline cluster — no gap unless a turn
    // boundary forced a new block.
    if (prev == SegmentKind::Tool && next == SegmentKind::Tool && !force) return;
    // Same-kind streaming coalesce (prose/text/thinking) skips gap unless forced.
    if (prev == next && !force) return;

    // Drop excess soft blanks (paragraph empties), then credit every trailing
    // empty visual row (BlankSegments + empty prose/echo-pad lines) so the
    // net gap is exactly `gap_rows` — never 0, never 2+.
    trim_trailing_soft_blanks();

    // Pop existing BlankSegments; empty prose/echo pads stay and count as credit.
    int blank_segs = 0;
    while (!segments_.empty()
           && dynamic_cast<BlankSegment*>(segments_.back().get())) {
        segments_.pop_back();
        ++blank_segs;
    }
    (void)blank_segs;

    const int credit = trailing_separator_rows();  // empty prose/echo pads only now
    if (credit > gap_rows) {
        // Too many trailing empties — peel down to exactly gap_rows.
        int excess = credit - gap_rows;
        if (auto* prose = dynamic_cast<ProseSegment*>(
                segments_.empty() ? nullptr : segments_.back().get())) {
            bool changed = false;
            while (excess > 0 && !prose->source_.empty()
                   && prose->source_.back().text.empty()) {
                prose->source_.pop_back();
                --excess;
                changed = true;
            }
            if (changed) prose->retheme();
        }
    }
    const int need = std::max(0, gap_rows - std::min(credit, gap_rows));
    for (int i = 0; i < need; ++i) append_blank_row();
}

void PaneScrollView::start_block() {
    start_block_gap(tui_design().layout.block_gap);
}

void PaneScrollView::start_block_gap(int gap_rows) {
    if (segments_.empty() || gap_rows < 0) return;
    int existing = 0;
    for (auto it = segments_.rbegin(); it != segments_.rend(); ++it) {
        if (!dynamic_cast<const BlankSegment*>(it->get())) break;
        ++existing;
    }
    while (existing > gap_rows) {
        segments_.pop_back();
        --existing;
    }
    for (int i = existing; i < gap_rows; ++i) append_blank_row();
}

void PaneScrollView::append(std::string_view text, bool new_block) {
    if (text.empty()) return;
    std::string chunk(text);
    const bool separate = new_block || last_content_kind() != SegmentKind::Text;
    if (separate) {
        ensure_block_gap(SegmentKind::Text, tui_design().layout.block_gap, new_block);
        while (!chunk.empty() && (chunk.front() == '\n' || chunk.front() == '\r')) {
            chunk.erase(chunk.begin());
        }
    }
    if (chunk.empty()) return;
    current_text().append(chunk);
}

void PaneScrollView::append_prose(const std::vector<StyledLine>& lines, bool new_block) {
    if (lines.empty()) return;
    const TuiDesign& d = tui_design();
    const bool separate = new_block || last_content_kind() != SegmentKind::Prose;
    std::vector<StyledLine> incoming = lines;
    if (separate) {
        ensure_block_gap(SegmentKind::Prose, d.layout.block_gap, new_block);
        // Leading blank prose lines would stack on top of the BlankSegment gap.
        while (!incoming.empty() && incoming.front().text.empty()
               && !arbiter::is_styled_user_echo_line(incoming.front())) {
            incoming.erase(incoming.begin());
        }
        if (incoming.empty()) return;
    }
    auto& prose = current_prose();
    auto prepared = prepare_prose_lines(std::move(incoming),
                                        wrap_cols_,
                                        d.layout.prose_paragraph_gap,
                                        trailing_empty_count(prose.source_));
    if (prepared.empty()) return;
    prose.append(prepared);
}

void PaneScrollView::append_code_open(std::string_view open_fence,
                                      std::string_view lang,
                                      size_t preview_rows,
                                      bool new_block) {
    if (open_fence.empty()) return;
    const TuiDesign& d = tui_design();
    const int gap = std::max(d.layout.block_gap, d.layout.panel_gap);
    ensure_block_gap(SegmentKind::Code, gap, new_block);
    auto& code = current_code();
    code.open(std::string(lang), preview_rows);
}

void PaneScrollView::append_code_line(std::string_view line) {
    current_code().append_line(std::string(line));
}

void PaneScrollView::append_code_close(std::string_view close_fence) {
    current_code().close(std::string(close_fence));
}

void PaneScrollView::append_blank_row() {
    segments_.push_back(std::make_unique<BlankSegment>());
}

void PaneScrollView::upsert_tool(const ToolActivityEvent& event, bool new_block) {
    if (event.id.empty() && event.label.empty()) return;

    // Prefer updating an unfinished row with the same id (live Started→Finished).
    // Never reopen a finished row — ids must be unique, but guard anyway so a
    // colliding Started cannot wipe prior-turn chrome.
    if (!event.id.empty()) {
        for (auto it = segments_.rbegin(); it != segments_.rend(); ++it) {
            if (auto* tool = dynamic_cast<ToolSegment*>(it->get())) {
                if (tool->id_ == event.id && !tool->finished_) {
                    tool->apply(event);
                    return;
                }
            }
        }
    }

    const TuiDesign& d = tui_design();
    ensure_block_gap(SegmentKind::Tool, d.layout.block_gap, new_block);
    auto seg = std::make_unique<ToolSegment>();
    seg->set_wrap_cols(wrap_cols_);
    seg->apply(event);
    segments_.push_back(std::move(seg));
}

void PaneScrollView::append_thinking(std::string_view delta,
                                     bool new_block,
                                     std::string_view agent_id) {
    if (delta.empty()) return;
    // Append into the most recent open ThinkingSegment when contiguous.
    if (!new_block && !segments_.empty()) {
        if (auto* think = dynamic_cast<ThinkingSegment*>(segments_.back().get())) {
            think->set_agent_id(agent_id);
            think->append(delta);
            return;
        }
        // Skip trailing blanks when looking for an open thinking block.
        for (auto it = segments_.rbegin(); it != segments_.rend(); ++it) {
            if (dynamic_cast<BlankSegment*>(it->get())) continue;
            if (auto* think = dynamic_cast<ThinkingSegment*>(it->get())) {
                think->set_agent_id(agent_id);
                think->append(delta);
                return;
            }
            break;
        }
    }
    const TuiDesign& d = tui_design();
    // Always leave block_gap before a new thinking box so tool rows / prose
    // don't kiss the top border (new_block alone is insufficient — tools do
    // not set need_sep for the following thinking stream).
    start_block_gap(d.layout.block_gap);
    auto seg = std::make_unique<ThinkingSegment>();
    seg->set_wrap_cols(wrap_cols_);
    seg->set_agent_id(agent_id);
    seg->append(delta);
    segments_.push_back(std::move(seg));
}

void PaneScrollView::append_diff(std::string_view patch) {
    if (patch.empty()) return;
    const int gap = std::max(tui_design().layout.block_gap, tui_design().layout.panel_gap);
#ifdef ARBITER_HAS_NATIVE_DIFF_VIEW
    DiffView native_probe;
    if (native_probe.set_patch(patch) && native_probe.valid()
        && native_probe.virtual_line_count() > 0) {
        ensure_block_gap(SegmentKind::Diff, gap, true);
        segments_.push_back(std::make_unique<NativeDiffSegment>(std::string(patch)));
        set_wrap_cols(wrap_cols_);
        return;
    }
#endif
    DiffPanel probe;
    probe.set_patch(patch);
    if (probe.visual_rows() > 0) {
        ensure_block_gap(SegmentKind::Diff, gap, true);
        segments_.push_back(std::make_unique<DiffSegment>(std::string(patch)));
        set_wrap_cols(wrap_cols_);
        return;
    }
    ensure_block_gap(SegmentKind::Text, gap, true);
    current_text().append(arbiter::render_diff_ansi(std::string(patch)));
}

void PaneScrollView::clear() {
    segments_.clear();
    segments_.push_back(std::make_unique<ProseSegment>());
}

void PaneScrollView::HistoryGapSegment::draw(OpenTuiHandle frame,
                                             int x,
                                             int y,
                                             int w,
                                             int h,
                                             int /*skip_rows*/) const {
    if (w <= 0 || h <= 0) return;
    const TuiDesign& d = tui_design();
    fill_rect(frame, x, y, w, 1, d.bg.base);
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "── %d earlier message%s · press PgUp to load ──",
                  remaining, remaining == 1 ? "" : "s");
    std::string text = trim_to_cells(buf, w);
    const int off = std::max(0, (w - cell_width(text)) / 2);
    draw_text(frame, x + off, y, text, d.text.subtle, d.bg.base);
}

bool PaneScrollView::has_gap() const {
    return !segments_.empty()
        && dynamic_cast<const HistoryGapSegment*>(segments_.front().get()) != nullptr;
}

int PaneScrollView::gap_remaining() const {
    if (segments_.empty()) return 0;
    if (const auto* gap = dynamic_cast<const HistoryGapSegment*>(segments_.front().get())) {
        return gap->remaining;
    }
    return 0;
}

void PaneScrollView::set_gap(int remaining) {
    const bool front_is_gap = has_gap();
    if (remaining <= 0) {
        if (front_is_gap) segments_.erase(segments_.begin());
        return;
    }
    if (front_is_gap) {
        static_cast<HistoryGapSegment&>(*segments_.front()).remaining = remaining;
    } else {
        segments_.insert(segments_.begin(), std::make_unique<HistoryGapSegment>(remaining));
    }
}

std::vector<std::unique_ptr<PaneScrollView::Segment>> PaneScrollView::take_segments() {
    auto out = std::move(segments_);
    segments_.clear();
    segments_.push_back(std::make_unique<ProseSegment>());
    return out;
}

void PaneScrollView::splice_front(std::vector<std::unique_ptr<Segment>> segs) {
    if (segs.empty()) return;
    for (auto& s : segs) s->set_wrap_cols(wrap_cols_);
    segments_.insert(segments_.begin(),
                     std::make_move_iterator(segs.begin()),
                     std::make_move_iterator(segs.end()));
}

void PaneScrollView::retheme() {
    for (auto& seg : segments_) {
        if (auto* prose = dynamic_cast<ProseSegment*>(seg.get())) {
            prose->retheme();
        } else if (auto* text = dynamic_cast<TextSegment*>(seg.get())) {
            text->retheme();
        } else if (auto* code = dynamic_cast<CodeSegment*>(seg.get())) {
            code->rehighlight();
        }
    }
}

bool PaneScrollView::toggle_code_block_in_view(int scroll_offset) {
    const int total = total_visual_rows();
    int first_visible = 0;
    if (total > viewport_h_) {
        first_visible = total - viewport_h_ - scroll_offset;
        if (first_visible < 0) first_visible = 0;
    }
    const int last_visible = first_visible + viewport_h_;

    CodeSegment* code_target = nullptr;
    ToolSegment* tool_target = nullptr;
    ThinkingSegment* think_target = nullptr;
    int row = 0;
    for (const auto& seg : segments_) {
        const int h = seg->visual_rows(wrap_cols_);
        const int seg_end = row + h;
        if (seg_end > first_visible && row < last_visible) {
            if (auto* think = dynamic_cast<ThinkingSegment*>(seg.get())) {
                if (think->can_expand()) {
                    think_target = think;
                    break;
                }
            }
            if (auto* tool = dynamic_cast<ToolSegment*>(seg.get())) {
                if (tool->can_expand()) {
                    tool_target = tool;
                    break;
                }
            }
            if (auto* code = dynamic_cast<CodeSegment*>(seg.get())) {
                if (code->is_truncated() || code->expanded_) {
                    code_target = code;
                    break;
                }
            }
        }
        row = seg_end;
    }
    if (think_target) {
        think_target->toggle_expanded();
        return true;
    }
    if (tool_target) {
        tool_target->toggle_expanded();
        return true;
    }
    if (!code_target) return false;
    code_target->toggle_expanded();
    return true;
}

int PaneScrollView::total_visual_rows() const {
    int total = 0;
    for (const auto& seg : segments_) {
        total += seg->visual_rows(wrap_cols_);
    }
    return total;
}

int PaneScrollView::max_scroll_offset() const {
    const int total = total_visual_rows();
    if (total <= viewport_h_) return 0;
    return total - viewport_h_;
}

std::vector<int> PaneScrollView::find_rows(const std::string& term) const {
    std::vector<int> out;
    if (term.empty()) return out;
    std::string needle = term;
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Legacy ANSI TextSegment echoes used a "> /find" caret prefix — skip those
    // so old scrollback does not self-match.  Prose user-echo `/find` lines are
    // skipped via Segment::find_skip_line (keeps row indices aligned).
    static constexpr std::string_view kLegacyEchoPrefix = "> /find";

    int base = 0;
    std::vector<std::string> lines;
    for (const auto& seg : segments_) {
        const int rows = seg->visual_rows(wrap_cols_);
        lines.clear();
        seg->collect_lines(lines);
        for (size_t k = 0; k < lines.size(); ++k) {
            if (seg->find_skip_line(k)) continue;
            std::string_view line_sv = lines[k];
            if (line_sv.size() >= kLegacyEchoPrefix.size()
                && line_sv.substr(0, kLegacyEchoPrefix.size()) == kLegacyEchoPrefix) {
                continue;
            }
            std::string hay = std::move(lines[k]);
            for (char& c : hay) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (hay.find(needle) == std::string::npos) continue;
            out.push_back(base + std::min(static_cast<int>(k), std::max(0, rows - 1)));
        }
        base += rows;
    }
    return out;
}

void PaneScrollView::draw(OpenTuiHandle frame,
                          TUI& tui,
                          int scroll_offset,
                          int new_while_scrolled) {
    // Degenerate / off-screen panes (zoom siblings, squeezed splits): do not
    // bind or paint — OpenTUI text-buffer draws are not safe with negative
    // origins from zero-size chrome math.
    if (frame == 0 || tui.cols() <= 0 || tui.scroll_region_rows() <= 0) return;

    bind(tui);

    if (buf_x_ < 0 || buf_y_ < 0 || viewport_w_ <= 0 || viewport_h_ <= 0) return;

    const TuiDesign& d = tui_design();
    const int pad = tui_pane_edge_pad(tui.cols(), d);
    const int gutter = std::max(0, std::min(d.layout.scroll_gutter_cols,
                                            std::max(0, tui.cols() - pad * 2 - 1)));
    if (gutter > 0) {
        const int gutter_x = tui.left_col() - 1 + pad;
        fill_rect(frame,
                  gutter_x,
                  buf_y_,
                  gutter,
                  viewport_h_,
                  d.bg.gutter);
    }

    const int total = total_visual_rows();
    int first_visible = 0;
    if (total > viewport_h_) {
        first_visible = total - viewport_h_ - scroll_offset;
        if (first_visible < 0) first_visible = 0;
    }

    bufferPushScissorRect(frame,
                          static_cast<int32_t>(buf_x_),
                          static_cast<int32_t>(buf_y_),
                          static_cast<std::uint32_t>(viewport_w_),
                          static_cast<std::uint32_t>(viewport_h_));

    int global_row = 0;
    int screen_y = 0;
    for (const auto& seg : segments_) {
        const int seg_h = seg->visual_rows(wrap_cols_);
        const int seg_end = global_row + seg_h;
        if (seg_end <= first_visible) {
            global_row = seg_end;
            continue;
        }
        const int skip_rows = std::max(0, first_visible - global_row);
        const int remaining = viewport_h_ - screen_y;
        if (remaining <= 0) break;
        const int draw_h = std::min(seg_h - skip_rows, remaining);
        seg->draw(frame,
                  buf_x_,
                  buf_y_ + screen_y,
                  viewport_w_,
                  draw_h,
                  skip_rows);
        screen_y += draw_h;
        global_row = seg_end;
        if (screen_y >= viewport_h_) break;
    }

    bufferPopScissorRect(frame);

    if (scroll_offset > 0) {
        char sbuf[96];
        if (new_while_scrolled > 0) {
            std::snprintf(sbuf, sizeof(sbuf),
                          "↑ %d rows above  ·  %d new  [PgDn]",
                          scroll_offset, new_while_scrolled);
        } else {
            std::snprintf(sbuf, sizeof(sbuf),
                          "↑ %d rows above  [PgDn to return]", scroll_offset);
        }
        tui.set_status(sbuf);
    }
}

} // namespace arbiter::opentui
