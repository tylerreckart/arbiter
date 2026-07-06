#include "tui/opentui/top_header_frame.h"

#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <cstdint>
#include <string_view>

namespace arbiter::opentui {

namespace {

void fill_rect(OpenTuiHandle frame,
               std::uint32_t x,
               std::uint32_t y,
               std::uint32_t w,
               std::uint32_t h,
               const TuiRgba& bg) {
    if (w == 0 || h == 0) return;
    bufferFillRect(frame, x, y, w, h, bg.data());
}

void draw_text(OpenTuiHandle frame,
               std::uint32_t x,
               std::uint32_t y,
               std::string_view text,
               const TuiRgba& fg,
               const TuiRgba& bg) {
    if (text.empty()) return;
    bufferDrawText(frame,
                   text.data(),
                   static_cast<std::uint32_t>(text.size()),
                   x,
                   y,
                   fg.data(),
                   bg.data(),
                   0);
}

} // namespace

void draw_top_header(OpenTuiHandle frame, int cols) {
    if (frame == 0 || cols <= 0) return;

    const TuiDesign& d = tui_design();
    const TuiRgba& bg = d.accent.primary;
    const TuiRgba fg = tui_sidebar_bg(d);

    fill_rect(frame, 0, 0, static_cast<std::uint32_t>(cols), 1, bg);
    draw_text(frame, 0, 0, "⛮", fg, bg);
}

} // namespace arbiter::opentui
