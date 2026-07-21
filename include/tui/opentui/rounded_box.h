#pragma once
// Shared rounded-corner box chrome for the input strip and sidebars.

#include "styled_text.h"
#include "tui/opentui/c_api.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace arbiter::opentui {

inline constexpr const char* kBoxCornerTL = "\u256D";  // ╭
inline constexpr const char* kBoxCornerTR = "\u256E";  // ╮
inline constexpr const char* kBoxCornerBR = "\u256F";  // ╯
inline constexpr const char* kBoxCornerBL = "\u2570";  // ╰

// Draw one horizontal border row (`╭──╮` when `top`, `╰──╯` otherwise).
// Optional `title` is painted into the run, framed by spaces so it reads as
// a break in the border.  Usable standalone by row-clipped renderers.
inline void draw_rounded_box_row(OpenTuiHandle frame,
                                 int x,
                                 int y,
                                 int w,
                                 bool top,
                                 const TuiRgba& border_fg,
                                 const TuiRgba& bg,
                                 std::string_view title = {},
                                 const TuiRgba* title_fg = nullptr) {
    if (frame == 0 || w < 2) return;

    const TuiDesign& d = tui_design();
    std::string run = top ? kBoxCornerTL : kBoxCornerBL;
    run.reserve(run.size()
                + static_cast<size_t>(std::max(0, w - 2))
                  * d.border.horizontal.size()
                + 4);
    for (int i = 0; i < w - 2; ++i) run += d.border.horizontal;
    run += top ? kBoxCornerTR : kBoxCornerBR;

    auto paint = [&](int dx, std::string_view text, const TuiRgba& fg) {
        if (text.empty()) return;
        bufferDrawText(frame,
                       text.data(),
                       static_cast<std::uint32_t>(text.size()),
                       static_cast<std::uint32_t>(dx),
                       static_cast<std::uint32_t>(y),
                       fg.data(),
                       bg.data(),
                       0);
    };

    paint(x, run, border_fg);

    if (title.empty() || w <= 4) return;
    const int budget = w - 4;  // corners + framing spaces
    std::string t = arbiter::trim_to_display_cols(std::string(title), budget);
    if (t.empty()) return;
    const std::string labeled = " " + t + " ";
    paint(x + 1, labeled, title_fg ? *title_fg : border_fg);
}

// Draw a rounded box. Optional `title` is painted into the top border,
// framed by spaces so it reads as a break in the horizontal run.
inline void draw_rounded_box(OpenTuiHandle frame,
                             int x,
                             int y,
                             int w,
                             int h,
                             const TuiRgba& border_fg,
                             const TuiRgba& bg,
                             std::string_view title = {},
                             const TuiRgba* title_fg = nullptr) {
    if (frame == 0 || w < 2 || h < 2) return;

    const TuiDesign& d = tui_design();
    auto paint = [&](int dx, int dy, std::string_view text, const TuiRgba& fg) {
        if (text.empty()) return;
        bufferDrawText(frame,
                       text.data(),
                       static_cast<std::uint32_t>(text.size()),
                       static_cast<std::uint32_t>(dx),
                       static_cast<std::uint32_t>(dy),
                       fg.data(),
                       bg.data(),
                       0);
    };

    draw_rounded_box_row(frame, x, y, w, /*top=*/true, border_fg, bg,
                         title, title_fg);
    for (int yy = y + 1; yy < y + h - 1; ++yy) {
        paint(x, yy, d.border.vertical, border_fg);
        paint(x + w - 1, yy, d.border.vertical, border_fg);
    }
    draw_rounded_box_row(frame, x, y + h - 1, w, /*top=*/false, border_fg, bg);
}

} // namespace arbiter::opentui
