#pragma once
// index/include/tui/scroll_buffer.h
//
// Bounded, ANSI-preserving, visual-row-aware history of model output.  Replaces
// the naive std::vector<std::string> the REPL used to hoard, which counted one
// wrapped logical line as one "row" and therefore miscounted scrollback once
// any line wrapped.
//
// What ScrollBuffer adds:
//   • A bounded ring (default 20k logical lines) — the old buffer grew forever.
//   • Per-line cached visual row count at the current column width.  The
//     cache is invalidated on set_cols(), so a terminal resize transparently
//     recomputes.
//   • ANSI-aware width: SGR escapes contribute zero visible columns, UTF-8
//     continuation bytes are not counted, so a colored wrapped paragraph's
//     row count matches what the terminal actually drew.
//   • render(top, bot, visual_offset) that clears the target region and writes
//     the right slice.  visual_offset is measured in visual rows from the tail
//     (0 = newest content aligned to the bottom of the region).

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>

namespace index_ai {

class ScrollBuffer {
public:
    explicit ScrollBuffer(size_t max_lines = 20000);

    // Append text; splits on '\n' and stores each segment as its own logical
    // line.  ANSI sequences are kept intact — they render naturally when the
    // line is written back out.  If the buffer exceeds max_lines after the
    // push, the oldest lines are evicted.
    void push(std::string_view text);

    // Drop everything.
    void clear();

    // Reset the column width used for wrap accounting.  Invalidates all cached
    // row counts so the next render picks up the new layout.
    void set_cols(int cols);
    int  cols() const { return cols_; }

    // Total visual rows across every stored line, at the current cols().
    int total_visual_rows() const;

    // Number of logical lines stored.
    size_t size() const { return lines_.size(); }

    // Render into the terminal region [top_row, bottom_row] (1-indexed, inclusive).
    // visual_offset is measured in visual rows above the tail; 0 = newest
    // content pinned to the bottom of the region.  Saves and restores the
    // cursor so readline's notion of position is not disturbed.
    //
    // The region is cleared before rendering.  If the most-recently-shown
    // logical line needs only part of its visual rows to fit, it is drawn in
    // full and the terminal clips the overflow — the content is still anchored
    // to the tail of the view.
    void render(int top_row, int bottom_row, int visual_offset) const;

private:
    struct Line {
        std::string text;
        mutable int cached_rows = -1;   // -1 = not yet computed for cols_
    };

    int rows_of(const Line& ln) const;
    static int visible_rows(std::string_view s, int cols);

    std::deque<Line> lines_;
    size_t           max_lines_;
    int              cols_ = 80;
};

} // namespace index_ai
