#include "tui/opentui/pane_scroll_view.h"

#include "markdown.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace arbiter::opentui {

namespace {

constexpr std::uint8_t kWrapWord = 2;

} // namespace

// --- TextSegment --------------------------------------------------------------

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
    segments_.push_back(std::make_unique<TextSegment>());
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
        if (const auto* text = dynamic_cast<const TextSegment*>(seg.get())) {
            if (!text->is_empty()) return true;
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
    segments_.push_back(std::make_unique<TextSegment>());
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
