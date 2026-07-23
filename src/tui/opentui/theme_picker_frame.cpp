#include "tui/opentui/theme_picker_frame.h"

#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace arbiter::opentui {

namespace {

constexpr std::uint32_t kAttrBold = 1u << 0;
constexpr int kMaxListRows = 12;
constexpr int kPanelWidth = 48;

void draw_text(OpenTuiHandle frame,
               std::uint32_t x,
               std::uint32_t y,
               std::string_view text,
               const TuiRgba& fg,
               const TuiRgba& bg,
               std::uint32_t attrs = 0) {
    if (text.empty()) return;
    bufferDrawText(frame,
                   text.data(),
                   static_cast<std::uint32_t>(text.size()),
                   x,
                   y,
                   fg.data(),
                   bg.data(),
                   attrs);
}

void fill_rect(OpenTuiHandle frame,
               std::uint32_t x,
               std::uint32_t y,
               std::uint32_t w,
               std::uint32_t h,
               const TuiRgba& bg) {
    if (w == 0 || h == 0) return;
    bufferFillRect(frame, x, y, w, h, bg.data());
}

std::string pad_to(std::string s, int w) {
    if (static_cast<int>(s.size()) > w) s.resize(static_cast<size_t>(w));
    s.resize(static_cast<size_t>(w), ' ');
    return s;
}

} // namespace

int theme_picker_visible_rows(const TUI& tui) {
    const int input_top = tui.input_top_row_pub();
    // Leave room for the header row above the list.
    return std::min({kMaxListRows, std::max(0, input_top - 3)});
}

void draw_theme_picker(OpenTuiHandle frame,
                       const ThemePickerSnapshot& snap,
                       const TUI& tui) {
    if (!snap.active || snap.themes.empty() || frame == 0) return;

    const TuiDesign& d = tui_design();
    const int cols = std::max(1, tui.cols());
    const int px = tui.left_col() - 1;
    const int input_top = tui.input_top_row_pub();
    const int n = static_cast<int>(snap.themes.size());
    const int list_rows = std::min({kMaxListRows, n, std::max(0, input_top - 3)});
    if (list_rows <= 0) return;

    const int top = input_top - list_rows - 1;
    if (top < 1) return;

    const int w = std::min({cols - 2, kPanelWidth, cols});
    const int x = px + 1;

    int first = snap.scroll_offset;
    first = std::clamp(first, 0, std::max(0, n - list_rows));

    fill_rect(frame,
              static_cast<std::uint32_t>(x),
              static_cast<std::uint32_t>(top),
              static_cast<std::uint32_t>(w),
              static_cast<std::uint32_t>(list_rows + 1),
              d.bg.header);

    const std::string header =
        " Themes  ↑↓ preview  Enter select  Esc cancel";
    draw_text(frame,
              static_cast<std::uint32_t>(x),
              static_cast<std::uint32_t>(top),
              pad_to(header, w),
              d.accent.primary,
              d.bg.header,
              kAttrBold);

    for (int i = 0; i < list_rows; ++i) {
        const int idx = first + i;
        if (idx >= n) break;
        const bool selected = idx == snap.selected;
        std::string row = selected ? " › " : "   ";
        row += snap.themes[static_cast<size_t>(idx)];
        if (selected) row += "  *";
        draw_text(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(top + 1 + i),
                  pad_to(std::move(row), w),
                  selected ? d.text.inverse : d.text.primary,
                  selected ? d.accent.primary : d.bg.header,
                  selected ? kAttrBold : 0);
    }
}

} // namespace arbiter::opentui
