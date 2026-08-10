#include "tui/opentui/menu_frame.h"

#include "styled_text.h"
#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace arbiter::opentui {

namespace {

constexpr std::uint32_t kAttrBold = 1u << 0;
constexpr int kMaxListRows = 14;
constexpr int kChromeRows = 1;  // title + key hints on one header row

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

std::string trim_cells(std::string s, int max_cells) {
    return arbiter::trim_to_display_cols(std::move(s), max_cells);
}

std::string pad_to(std::string s, int w) {
    if (static_cast<int>(s.size()) > w) s.resize(static_cast<size_t>(w));
    s.resize(static_cast<size_t>(w), ' ');
    return s;
}

void paint_menu_body(OpenTuiHandle frame,
                     const MenuSnapshot& snap,
                     int x,
                     int y,
                     int w,
                     int list_rows) {
    if (frame == 0 || w < 6 || list_rows < 0) return;

    const TuiDesign& d = tui_menu_design();
    const int n = static_cast<int>(snap.items.size());
    const int h = list_rows + kChromeRows;
    if (h < 1) return;

    // Bright menu surface over the dimmed TUI.
    const TuiRgba& surface = d.bg.header;
    fill_rect(frame,
              static_cast<std::uint32_t>(x),
              static_cast<std::uint32_t>(y),
              static_cast<std::uint32_t>(w),
              static_cast<std::uint32_t>(h),
              surface);

    std::string header = " " + snap.title;
    if (!snap.hint.empty()) {
        header += "  ";
        header += snap.hint;
    }
    draw_text(frame,
              static_cast<std::uint32_t>(x),
              static_cast<std::uint32_t>(y),
              pad_to(trim_cells(std::move(header), w), w),
              d.accent.primary,
              surface,
              kAttrBold);

    int first = snap.scroll_offset;
    first = std::clamp(first, 0, std::max(0, n - list_rows));

    for (int i = 0; i < list_rows; ++i) {
        const int idx = first + i;
        if (idx >= n) break;
        const auto& it = snap.items[static_cast<size_t>(idx)];
        const int row_y = y + 1 + i;
        const bool selected = idx == snap.selected && !it.section && !it.disabled;

        if (it.section) {
            draw_text(frame,
                      static_cast<std::uint32_t>(x),
                      static_cast<std::uint32_t>(row_y),
                      pad_to(trim_cells(" " + it.label, w), w),
                      d.accent.primary,
                      surface,
                      kAttrBold);
            continue;
        }

        // Selection is a leading marker only — no row background fill.
        std::string line;
        line += selected ? " › " : "   ";
        if (!it.shortcut.empty()) {
            line += it.shortcut;
            line += " ";
        }
        line += it.label;
        if (it.current) line += "  *";
        if (!it.detail.empty()) {
            line += "  ";
            line += it.detail;
        }

        const TuiRgba& row_fg = it.destructive ? d.accent.error
                              : it.disabled    ? d.text.subtle
                              : selected       ? d.accent.primary
                                               : d.text.primary;
        draw_text(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(row_y),
                  pad_to(trim_cells(std::move(line), w), w),
                  row_fg,
                  surface,
                  selected ? kAttrBold : 0);
    }
}

} // namespace

int menu_visible_rows(const TUI& tui, int item_count) {
    const int input_top = tui.input_top_row_pub();
    const int budget = std::max(0, input_top - 1 - kChromeRows);
    return std::min({kMaxListRows, std::max(0, item_count), budget});
}

void draw_menu(OpenTuiHandle frame,
               const MenuSnapshot& snap,
               const TUI& tui) {
    if (!snap.active || snap.items.empty() || frame == 0) return;

    const int cols = std::max(1, tui.cols());
    const int px = tui.left_col() - 1;
    const int input_top = tui.input_top_row_pub();
    const int n = static_cast<int>(snap.items.size());
    const int list_rows = menu_visible_rows(tui, n);
    if (list_rows <= 0) return;

    const int h = list_rows + kChromeRows;
    const int top = input_top - h;
    if (top < 1) return;

    const int w = std::min({cols - 2, std::max(24, snap.max_width), cols});
    if (w < 6) return;
    const int x = px + 1;
    paint_menu_body(frame, snap, x, top, w, list_rows);
}

void draw_menu_at(OpenTuiHandle frame,
                  const MenuSnapshot& snap,
                  int x,
                  int y,
                  int w,
                  int max_bottom_y) {
    if (!snap.active || snap.items.empty() || frame == 0 || w < 6) return;
    const int n = static_cast<int>(snap.items.size());
    const int max_h = std::max(0, max_bottom_y - y + 1);
    const int list_budget = std::max(0, max_h - kChromeRows);
    const int list_rows = std::min({kMaxListRows, n, list_budget});
    if (list_rows <= 0) return;
    paint_menu_body(frame, snap, x, y, w, list_rows);
}

} // namespace arbiter::opentui
