#include "tui/opentui/pane_scroll_view.h"

#include "markdown.h"
#include "tui/style_resolver.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace arbiter::opentui {

namespace {

constexpr std::uint8_t kWrapWord = 2;

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

void PaneScrollView::ProseSegment::append(const std::vector<StyledLine>& lines) {
    if (!span_append_) return;
    for (const StyledLine& line : lines) {
        span_append_->append_line(line);
    }
}

void PaneScrollView::ProseSegment::clear() {
    if (span_append_) span_append_->clear();
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
    wrap_cols_ = std::max(1, cols);
    if (view_ != 0) {
        textBufferViewSetWrapWidth(view_, static_cast<std::uint32_t>(wrap_cols_));
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
    styled_append_->append(text);
}

void PaneScrollView::TextSegment::clear() {
    if (styled_append_) styled_append_->clear();
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

// --- CodeSegment (draw-time collapsed code blocks) ---------------------------

void PaneScrollView::CodeSegment::open(std::string open_fence, size_t preview_rows) {
    open_fence_ = std::move(open_fence);
    preview_rows_ = preview_rows;
    lines_.clear();
    close_fence_.clear();
    closed_ = false;
    cached_rows_ = -1;
}

void PaneScrollView::CodeSegment::append_line(std::string line) {
    lines_.push_back(std::move(line));
    cached_rows_ = -1;
}

void PaneScrollView::CodeSegment::close(std::string close_fence) {
    close_fence_ = std::move(close_fence);
    closed_ = true;
    cached_rows_ = -1;
}

size_t PaneScrollView::CodeSegment::visible_body_count() const {
    if (preview_rows_ == 0) return lines_.size();
    return std::min(lines_.size(), preview_rows_);
}

int PaneScrollView::CodeSegment::visual_rows(int /*content_w*/) const {
    if (cached_rows_ >= 0) return cached_rows_;
    int rows = 1;
    rows += static_cast<int>(visible_body_count());
    if (preview_rows_ > 0 && lines_.size() > preview_rows_) rows += 1;
    if (closed_ && !close_fence_.empty()) rows += 1;
    cached_rows_ = rows;
    return rows;
}

void PaneScrollView::CodeSegment::set_wrap_cols(int /*cols*/) {
    cached_rows_ = -1;
    cached_width_ = -1;
}

void PaneScrollView::CodeSegment::draw(OpenTuiHandle frame,
                                       int x,
                                       int y,
                                       int w,
                                       int /*h*/,
                                       int skip_rows) const {
    if (w <= 0) return;

    auto draw_text_row = [&](const std::string& text, StyleId id, int& screen_row) {
        if (screen_row < skip_rows) {
            ++screen_row;
            return;
        }
        const ResolvedStyle rs = resolve_style(id);
        bufferDrawText(frame,
                       text.data(),
                       static_cast<std::uint32_t>(text.size()),
                       static_cast<std::uint32_t>(x),
                       static_cast<std::uint32_t>(y + (screen_row - skip_rows)),
                       rs.fg ? rs.fg->data() : nullptr,
                       nullptr,
                       rs.attrs);
        ++screen_row;
    };

    int row = 0;
    draw_text_row(open_fence_, StyleId::Dim, row);

    const size_t show = visible_body_count();
    for (size_t i = 0; i < show; ++i) {
        draw_text_row(lines_[i], StyleId::Code, row);
    }

    if (preview_rows_ > 0 && lines_.size() > preview_rows_) {
        const std::string summary =
            "  … (code block, " + std::to_string(lines_.size()) + "+ lines) …";
        draw_text_row(summary, StyleId::Dim, row);
    }

    if (closed_ && !close_fence_.empty()) {
        draw_text_row(close_fence_, StyleId::Dim, row);
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
    start_block();
    auto seg = std::make_unique<CodeSegment>();
    auto& ref = *seg;
    segments_.push_back(std::move(seg));
    return ref;
}

void PaneScrollView::bind(const TUI& tui) {
    const TuiDesign& d = tui_design();
    const int raw_pad = (tui.cols() <= d.layout.dense_cols)
        ? 0
        : std::max(0, d.layout.pane_padding_x);
    const int pad = std::min(raw_pad, std::max(0, (tui.cols() - 1) / 2));
    const int content_w = std::max(1, tui.cols() - (pad * 2));

    buf_x_ = tui.left_col() - 1 + pad;
    buf_y_ = tui.scroll_top_row() - 1;
    viewport_w_ = content_w;
    viewport_h_ = tui.scroll_region_rows();
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
    if (segments_.empty()) return;
    if (dynamic_cast<const BlankSegment*>(segments_.back().get())) return;
    append_blank_row();
}

void PaneScrollView::append(std::string_view text, bool new_block) {
    if (text.empty()) return;
    std::string chunk(text);
    if (new_block || !has_rendered_content()) {
        start_block();
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
    if (new_block || !has_rendered_content()) {
        start_block();
    }
    current_prose().append(lines);
}

void PaneScrollView::append_code_open(std::string_view open_fence,
                                      size_t preview_rows,
                                      bool new_block) {
    if (open_fence.empty()) return;
    if (new_block || !has_rendered_content()) {
        start_block();
    }
    auto& code = current_code();
    code.open(std::string(open_fence), preview_rows);
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
#ifdef ARBITER_HAS_NATIVE_DIFF_VIEW
    DiffView native_probe;
    if (native_probe.set_patch(patch) && native_probe.valid()
        && native_probe.virtual_line_count() > 0) {
        start_block();
        segments_.push_back(std::make_unique<NativeDiffSegment>(std::string(patch)));
        set_wrap_cols(wrap_cols_);
        return;
    }
#endif
    DiffPanel probe;
    probe.set_patch(patch);
    if (probe.visual_rows() > 0) {
        start_block();
        segments_.push_back(std::make_unique<DiffSegment>(std::string(patch)));
        set_wrap_cols(wrap_cols_);
        return;
    }
    start_block();
    current_text().append(arbiter::render_diff_ansi(std::string(patch)));
}

void PaneScrollView::clear() {
    segments_.clear();
    segments_.push_back(std::make_unique<ProseSegment>());
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

void PaneScrollView::draw(OpenTuiHandle frame,
                          TUI& tui,
                          int scroll_offset,
                          int new_while_scrolled) {
    bind(tui);

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
