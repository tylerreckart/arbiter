// index_ai/src/tui/scroll_buffer.cpp — see tui/scroll_buffer.h

#include "tui/scroll_buffer.h"

#include <algorithm>
#include <cstdio>

namespace index_ai {

ScrollBuffer::ScrollBuffer(size_t max_lines) : max_lines_(max_lines) {}

void ScrollBuffer::push(std::string_view text) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            if (start < text.size())
                lines_.push_back({std::string(text.substr(start)), -1});
            break;
        }
        lines_.push_back({std::string(text.substr(start, nl - start)), -1});
        start = nl + 1;
    }
    while (lines_.size() > max_lines_) lines_.pop_front();
}

void ScrollBuffer::clear() {
    lines_.clear();
}

void ScrollBuffer::set_cols(int cols) {
    if (cols == cols_) return;
    cols_ = cols;
    for (auto& ln : lines_) ln.cached_rows = -1;
}

int ScrollBuffer::total_visual_rows() const {
    int total = 0;
    for (auto& ln : lines_) total += rows_of(ln);
    return total;
}

int ScrollBuffer::rows_of(const Line& ln) const {
    if (ln.cached_rows < 0)
        ln.cached_rows = visible_rows(ln.text, cols_);
    return ln.cached_rows;
}

// Count how many terminal rows a string occupies at `cols` width.
// Skips SGR escape sequences (\033[...<letter>) and UTF-8 continuation bytes,
// so the answer matches what the terminal actually draws.  Empty strings
// contribute a single blank row.
int ScrollBuffer::visible_rows(std::string_view s, int cols) {
    if (cols <= 0) return 1;
    int visible = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
            // Skip CSI sequence: everything up to a byte in 0x40..0x7E.
            i += 2;
            while (i < s.size()) {
                unsigned char x = (unsigned char)s[i++];
                if (x >= 0x40 && x <= 0x7E) break;
            }
            continue;
        }
        // UTF-8 continuation byte → same code point as prior byte, skip.
        if ((c & 0xC0) == 0x80) { ++i; continue; }
        ++visible;
        ++i;
    }
    if (visible == 0) return 1;
    return (visible + cols - 1) / cols;
}

namespace {
// Emit one logical line into the target rect, wrapping at `width` visible
// columns.  Returns the number of pane-rows consumed.  ANSI SGR escapes
// pass through without counting; UTF-8 continuation bytes are emitted as
// part of their lead byte without advancing the column counter.  Stops
// when `max_rows` is reached (caller is responsible for clipping).
int write_wrapped_line(std::string_view text,
                        int left_col, int top_row,
                        int width, int max_rows) {
    if (max_rows <= 0 || width <= 0) return 0;
    int cur_row = top_row;
    int col_used = 0;
    std::printf("\033[%d;%dH", cur_row, left_col);
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = (unsigned char)text[i];
        // Pass-through CSI escapes — don't count toward column budget.
        if (c == 0x1B && i + 1 < text.size() && text[i + 1] == '[') {
            size_t start = i;
            i += 2;
            while (i < text.size()) {
                unsigned char x = (unsigned char)text[i++];
                if (x >= 0x40 && x <= 0x7E) break;
            }
            std::fwrite(text.data() + start, 1, i - start, stdout);
            continue;
        }
        // UTF-8 continuation byte — emit as part of prior code point.
        if ((c & 0xC0) == 0x80) {
            std::putc(static_cast<char>(c), stdout);
            ++i;
            continue;
        }
        // Start of a new visible column.
        if (col_used >= width) {
            if (cur_row - top_row + 1 >= max_rows) break;
            ++cur_row;
            col_used = 0;
            std::printf("\033[%d;%dH", cur_row, left_col);
        }
        std::putc(static_cast<char>(c), stdout);
        ++col_used;
        ++i;
    }
    return cur_row - top_row + 1;
}
} // namespace

void ScrollBuffer::render(int left_col, int top_row, int width,
                          int bottom_row, int visual_offset) const {
    int H = bottom_row - top_row + 1;
    if (H <= 0 || width <= 0) return;

    std::printf("\0337\033[?25l");   // save cursor

    // Clear the region, pane-aware: write exactly `width` spaces per row
    // starting at left_col.  \033[2K would clear the whole physical line
    // and wipe sibling panes.
    std::string blank(width, ' ');
    for (int r = top_row; r <= bottom_row; ++r) {
        std::printf("\033[%d;%dH\033[0m", r, left_col);
        std::fwrite(blank.data(), 1, blank.size(), stdout);
    }

    // Window selection — [want_top_v, want_bot_v) in "visual rows from tail".
    int want_bot_v = visual_offset;
    int want_top_v = visual_offset + H;

    int start_idx = 0;
    int end_idx   = 0;
    int acc       = 0;

    bool found_end = false;
    for (int i = (int)lines_.size() - 1; i >= 0; --i) {
        int vr = rows_of(lines_[i]);
        if (!found_end) {
            if (acc + vr > want_bot_v) {
                end_idx = i + 1;
                found_end = true;
            }
        }
        acc += vr;
        if (found_end && acc >= want_top_v) {
            start_idx = i;
            break;
        }
    }
    if (!found_end) {
        std::printf("\0338\033[?25h");
        std::fflush(stdout);
        return;
    }
    if (acc < want_top_v) start_idx = 0;

    // Render [start_idx, end_idx) into the region, anchored at top_row.
    // Each logical line wraps manually at `width` — bare \n or \r\n between
    // them wouldn't work here because we need to reposition to left_col on
    // every new row, not physical column 1.
    int rows_written = 0;
    for (int i = start_idx; i < end_idx && rows_written < H; ++i) {
        rows_written += write_wrapped_line(lines_[i].text,
                                            left_col, top_row + rows_written,
                                            width, H - rows_written);
    }

    std::printf("\0338\033[?25h");   // restore cursor
    std::fflush(stdout);
}

} // namespace index_ai
