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

void ScrollBuffer::render(int top_row, int bottom_row, int visual_offset) const {
    int H = bottom_row - top_row + 1;
    if (H <= 0) return;

    std::printf("\0337");   // save cursor

    // Clear the region.
    for (int r = top_row; r <= bottom_row; ++r)
        std::printf("\033[%d;1H\033[2K", r);

    // Walk from the tail, accumulating visual rows until we have H rows
    // worth of content that's above `visual_offset`.
    //
    // Window: [want_top_v, want_bot_v) in "visual rows from the tail"
    //   want_bot_v = visual_offset          (exclusive bottom)
    //   want_top_v = visual_offset + H      (exclusive top)
    int want_bot_v = visual_offset;
    int want_top_v = visual_offset + H;

    int start_idx = 0;
    int end_idx   = 0;
    int acc       = 0;   // visual rows accumulated walking back

    // Find end_idx (exclusive): first index_ai where acc ≥ want_bot_v.
    bool found_end = false;
    for (int i = (int)lines_.size() - 1; i >= 0; --i) {
        int vr = rows_of(lines_[i]);
        if (!found_end) {
            if (acc + vr > want_bot_v) {
                end_idx = i + 1;   // include line i; it crosses the bottom
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
        // Entire buffer is below visual_offset — nothing to show.
        std::printf("\0338");
        std::fflush(stdout);
        return;
    }
    if (acc < want_top_v) {
        // Hit the start of the buffer first.
        start_idx = 0;
    }

    // Render [start_idx, end_idx) into the region, anchored at top_row.
    // Between-line separator is CRLF, not bare LF.  Readline puts the terminal
    // in raw output mode (OPOST cleared on some platforms) so a bare '\n'
    // moves the cursor down without resetting the column — that's how stray
    // characters used to leak from the scroll region into the input row.
    std::printf("\033[%d;1H", top_row);
    for (int i = start_idx; i < end_idx; ++i) {
        if (i > start_idx) std::fputs("\r\n", stdout);
        std::fwrite(lines_[i].text.data(), 1, lines_[i].text.size(), stdout);
    }

    std::printf("\0338");   // restore cursor
    std::fflush(stdout);
}

} // namespace index_ai
