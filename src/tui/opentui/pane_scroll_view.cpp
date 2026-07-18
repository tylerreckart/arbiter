#include "tui/opentui/pane_scroll_view.h"

#include "code_highlighter.h"
#include "markdown.h"
#include "styled_text.h"
#include "tui/ansi_util.h"
#include "tui/style_resolver.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
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
    if (w <= 0 || h <= 0) return;
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
    if (text.empty()) return;
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
        span_append_->append_line(arbiter::pad_styled_user_echo_line(line, wrap_cols_));
    } else {
        span_append_->append_line(line);
    }
}

void PaneScrollView::ProseSegment::append(const std::vector<StyledLine>& lines) {
    if (!span_append_) return;
    for (const StyledLine& line : lines) {
        source_.push_back(line);
        emit_line(line);
    }
}

void PaneScrollView::ProseSegment::clear() {
    source_.clear();
    if (span_append_) span_append_->clear();
}

void PaneScrollView::ProseSegment::retheme() {
    if (!span_append_) return;
    span_append_->clear();
    for (const StyledLine& line : source_) {
        emit_line(line);
    }
}

bool PaneScrollView::ProseSegment::is_empty() const {
    return buffer_ == 0 || textBufferGetLength(buffer_) == 0;
}

int PaneScrollView::ProseSegment::visual_rows(int content_w) const {
    if (view_ == 0) return 0;
    const_cast<ProseSegment*>(this)->wrap_cols_ = content_w;
    textBufferViewSetWrapWidth(view_, static_cast<std::uint32_t>(content_w));
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
    for (const auto& line : source_) {
        if (arbiter::is_styled_user_echo_line(line)) {
            // Skip the echoed "/find …" command itself (UI chrome, not content).
            // Do not skip model/prose lines that happen to start with "/find".
            std::string_view payload = line.text;
            static constexpr std::string_view kFind = "/find";
            if (payload.size() >= kFind.size()
                && payload.substr(0, kFind.size()) == kFind
                && (payload.size() == kFind.size() || payload[kFind.size()] == ' ')) {
                continue;
            }
        }
        out.push_back(line.text);
    }
}

void PaneScrollView::ProseSegment::draw(OpenTuiHandle frame,
                                        int x,
                                        int y,
                                        int w,
                                        int h,
                                        int skip_rows) const {
    if (view_ == 0 || w <= 0 || h <= 0) return;
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
    if (view_ == 0) return 0;
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
    if (view_ == 0 || w <= 0 || h <= 0) return;
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
    if (patch_.empty() || w <= 0 || h <= 0 || !diff_.valid()) return;
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
    start_block_gap(tui_design().layout.panel_gap);
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

void PaneScrollView::start_block() {
    start_block_gap(tui_design().layout.block_gap);
}

void PaneScrollView::start_block_gap(int gap_rows) {
    if (segments_.empty() || gap_rows <= 0) return;
    int existing = 0;
    for (auto it = segments_.rbegin(); it != segments_.rend(); ++it) {
        if (!dynamic_cast<const BlankSegment*>(it->get())) break;
        ++existing;
    }
    for (int i = existing; i < gap_rows; ++i) append_blank_row();
}

void PaneScrollView::append(std::string_view text, bool new_block) {
    if (text.empty()) return;
    std::string chunk(text);
    if (new_block || !has_rendered_content()) {
        start_block_gap(tui_design().layout.block_gap);
        if (new_block) {
            while (!chunk.empty() && (chunk.front() == '\n' || chunk.front() == '\r')) {
                chunk.erase(chunk.begin());
            }
        }
    }
    if (chunk.empty()) return;
    current_text().append(chunk);
}

void PaneScrollView::append_prose(const std::vector<StyledLine>& lines, bool new_block) {
    if (lines.empty()) return;
    const TuiDesign& d = tui_design();
    if (new_block || !has_rendered_content()) {
        start_block_gap(d.layout.block_gap);
    }
    auto& prose = current_prose();
    auto prepared = prepare_prose_lines(lines,
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
    if (new_block || !has_rendered_content()) {
        start_block_gap(std::max(d.layout.block_gap, d.layout.panel_gap));
    } else {
        start_block_gap(d.layout.panel_gap);
    }
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

void PaneScrollView::append_diff(std::string_view patch) {
    if (patch.empty()) return;
    const int gap = tui_design().layout.panel_gap;
#ifdef ARBITER_HAS_NATIVE_DIFF_VIEW
    DiffView native_probe;
    if (native_probe.set_patch(patch) && native_probe.valid()
        && native_probe.virtual_line_count() > 0) {
        start_block_gap(gap);
        segments_.push_back(std::make_unique<NativeDiffSegment>(std::string(patch)));
        set_wrap_cols(wrap_cols_);
        return;
    }
#endif
    DiffPanel probe;
    probe.set_patch(patch);
    if (probe.visual_rows() > 0) {
        start_block_gap(gap);
        segments_.push_back(std::make_unique<DiffSegment>(std::string(patch)));
        set_wrap_cols(wrap_cols_);
        return;
    }
    start_block_gap(gap);
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

    CodeSegment* target = nullptr;
    int row = 0;
    for (const auto& seg : segments_) {
        const int h = seg->visual_rows(wrap_cols_);
        const int seg_end = row + h;
        if (seg_end > first_visible && row < last_visible) {
            if (auto* code = dynamic_cast<CodeSegment*>(seg.get())) {
                if (code->is_truncated() || code->expanded_) {
                    target = code;
                    break;
                }
            }
        }
        row = seg_end;
    }
    if (!target) return false;
    target->toggle_expanded();
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

    // ProseSegment::collect_lines already omits echoed "/find …" user turns.
    // Legacy ANSI TextSegment echoes used a "> /find" caret prefix — skip those
    // here so old scrollback does not self-match.
    static constexpr std::string_view kLegacyEchoPrefix = "> /find";

    int base = 0;
    std::vector<std::string> lines;
    for (const auto& seg : segments_) {
        const int rows = seg->visual_rows(wrap_cols_);
        lines.clear();
        seg->collect_lines(lines);
        for (size_t k = 0; k < lines.size(); ++k) {
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
    bind(tui);

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
