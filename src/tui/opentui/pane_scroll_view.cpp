#include "tui/opentui/pane_scroll_view.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace arbiter::opentui {

namespace {

constexpr std::uint8_t kWrapWord = 2;

} // namespace

PaneScrollView::PaneScrollView() {
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
}

PaneScrollView::~PaneScrollView() {
    if (view_ != 0) destroyTextBufferView(view_);
    if (buffer_ != 0) destroyTextBuffer(buffer_);
}

void PaneScrollView::bind(const TUI& tui) {
    buf_x_ = tui.left_col() - 1;
    buf_y_ = tui.scroll_top_row() - 1;
    viewport_w_ = tui.cols();
    viewport_h_ = tui.scroll_region_rows();
    set_wrap_cols(tui.cols());
}

void PaneScrollView::set_wrap_cols(int cols) {
    wrap_cols_ = std::max(1, cols);
    textBufferViewSetWrapWidth(view_, static_cast<std::uint32_t>(wrap_cols_));
}

void PaneScrollView::append(std::string_view text) {
    if (text.empty()) return;
    std::string plain = strip_stream_.feed(text);
    if (plain.empty()) return;
    text_storage_.push_back(std::move(plain));
    const std::string& stable = text_storage_.back();
    textBufferAppend(buffer_, stable.data(), static_cast<std::uint32_t>(stable.size()));
}

void PaneScrollView::clear() {
    textBufferClear(buffer_);
    text_storage_.clear();
    strip_stream_.reset();
}

int PaneScrollView::total_visual_rows() const {
    return static_cast<int>(textBufferViewGetVirtualLineCount(view_));
}

int PaneScrollView::max_scroll_offset() const {
    const int total = total_visual_rows();
    if (total <= viewport_h_) return 0;
    return total - viewport_h_;
}

void PaneScrollView::sync_scroll_offset(int scroll_offset) const {
    const int total = total_visual_rows();
    int start_y = 0;
    if (total > viewport_h_) {
        start_y = total - viewport_h_ - scroll_offset;
        if (start_y < 0) start_y = 0;
    }
    // Viewport x/y index into virtual lines (0 = top of content).  Screen
    // placement is handled by bufferDrawTextBufferView's x/y arguments.
    // first_line_offset is for partial first-line wrap width only — not scroll.
    textBufferViewSetFirstLineOffset(view_, 0);
    textBufferViewSetViewport(view_,
                              0,
                              static_cast<std::uint32_t>(start_y),
                              static_cast<std::uint32_t>(viewport_w_),
                              static_cast<std::uint32_t>(viewport_h_));
}

void PaneScrollView::draw(OpenTuiHandle frame,
                          TUI& tui,
                          int scroll_offset,
                          int new_while_scrolled) {
    bind(tui);
    sync_scroll_offset(scroll_offset);

    bufferPushScissorRect(frame,
                          static_cast<int32_t>(buf_x_),
                          static_cast<int32_t>(buf_y_),
                          static_cast<std::uint32_t>(viewport_w_),
                          static_cast<std::uint32_t>(viewport_h_));
    bufferDrawTextBufferView(frame, view_,
                             static_cast<std::uint32_t>(buf_x_),
                             static_cast<std::uint32_t>(buf_y_));
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
