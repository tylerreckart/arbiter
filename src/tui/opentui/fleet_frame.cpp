#include "tui/opentui/fleet_frame.h"

#include "styled_text.h"
#include "tui/opentui/engine.h"
#include "tui/opentui/rounded_box.h"
#include "tui/sidebar_format.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace arbiter::opentui {

namespace {

constexpr std::uint32_t kAttrBold = 1u << 0;
constexpr int kBoxPad = 1;

int cell_width(std::string_view s) {
    return static_cast<int>(arbiter::display_width(s));
}

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

std::string trim_to_cells(std::string s, int max_cells) {
    return arbiter::trim_to_display_cols(std::move(s), max_cells);
}

int scroll_bottom_y(const Rect& pane_rect, int pane_input_rows,
                    int pane_bottom_pad_rows) {
    const int bottom_pad = std::max(1, pane_bottom_pad_rows);
    const int sep_top = pane_rect.y + pane_rect.h - bottom_pad - pane_input_rows
                      - TUI::kSepRows;
    return sep_top - 1;
}

}  // namespace

int fleet_sidebar_list_top(const Rect& r, int overlay_lines) {
    // Blank row above the box, title border, blank row, optional overlay.
    int y = r.y + 3;
    if (overlay_lines > 0) y += overlay_lines + 1;  // overlay + gap
    return y;
}

int fleet_sidebar_visible_rows(const Rect& sidebar_rect,
                               const Rect& pane_rect,
                               int pane_input_rows,
                               int overlay_lines,
                               int pane_bottom_pad_rows) {
    const int top = fleet_sidebar_list_top(sidebar_rect, overlay_lines);
    const int bottom = scroll_bottom_y(pane_rect, pane_input_rows,
                                       pane_bottom_pad_rows);
    return std::max(0, bottom - top + 1);
}

void draw_fleet_sidebar(OpenTuiHandle frame,
                        const FleetSnapshot& snap,
                        const Rect& r,
                        const Rect& pane_rect,
                        int pane_input_rows,
                        int pane_bottom_pad_rows) {
    if (frame == 0 || r.w <= 0 || r.h <= 0) return;
    if (pane_rect.h <= 0) return;

    const TuiDesign& d = tui_design();
    const SidebarColors sc = tui_sidebar_colors(d);
    const int bottom_pad = std::max(1, pane_bottom_pad_rows);
    const int panel_top_y = r.y + 1;
    const int input_bottom_y = pane_rect.y + pane_rect.h - bottom_pad - 1;
    if (input_bottom_y < panel_top_y + 1) return;

    const int block_x = r.x;
    const int block_w = r.w;
    const int content_x = block_x + 1 + kBoxPad;
    const int content_w = std::max(1, block_w - 2 - (kBoxPad * 2));
    const int block_h = std::max(2, input_bottom_y - panel_top_y + 1);
    const TuiRgba& bg = d.bg.scroll;
    const TuiRgba& border = snap.focused ? d.border.focus : d.text.muted;
    const TuiRgba* title_fg = snap.focused ? &d.accent.primary : &d.accent.primary;

    draw_rounded_box(frame,
                     block_x,
                     panel_top_y,
                     block_w,
                     block_h,
                     border,
                     bg,
                     "Fleet",
                     title_fg);

    int y = panel_top_y + 2;
    const int list_bottom = scroll_bottom_y(pane_rect, pane_input_rows,
                                            pane_bottom_pad_rows);

    const auto overlay_lines = format_fleet_overlay(snap.overlay, content_w);
    for (const auto& line : overlay_lines) {
        if (y > list_bottom) return;
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x),
                  static_cast<std::uint32_t>(y),
                  trim_to_cells(line, content_w),
                  sc.body,
                  bg);
        ++y;
    }
    if (!overlay_lines.empty() && y <= list_bottom) {
        ++y;  // gap before the tree
    }

    if (snap.rows.empty()) {
        if (y <= list_bottom) {
            draw_text(frame,
                      static_cast<std::uint32_t>(content_x),
                      static_cast<std::uint32_t>(y),
                      trim_to_cells("(no agents)", content_w),
                      sc.label,
                      bg);
        }
        return;
    }

    const int vis = std::max(0, list_bottom - y + 1);
    int idx = std::max(0, snap.scroll_offset);
    const int sel = snap.selected;
    int painted = 0;
    while (idx < static_cast<int>(snap.rows.size()) && painted < vis) {
        const auto& row = snap.rows[static_cast<size_t>(idx)];
        const bool selected = snap.focused && idx == sel;
        const TuiRgba row_bg = selected ? d.bg.status : bg;
        const TuiRgba& fg = row.running ? d.accent.primary
                          : (row.ended && !row.ok) ? d.accent.error
                          : (row.ended) ? d.accent.success
                          : sc.body;
        if (selected) {
            bufferFillRect(frame,
                           static_cast<std::uint32_t>(content_x - 1),
                           static_cast<std::uint32_t>(y),
                           static_cast<std::uint32_t>(std::max(1, content_w + 1)),
                           1,
                           row_bg.data());
        }
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x),
                  static_cast<std::uint32_t>(y),
                  trim_to_cells(row.line, content_w),
                  selected ? d.text.primary : fg,
                  row_bg,
                  selected ? kAttrBold : 0);
        ++y;
        ++idx;
        ++painted;
    }

    if (snap.total_input + snap.total_output > 0) {
        const int hint_y = pane_rect.y + pane_rect.h - 2;
        if (hint_y > input_bottom_y) {
            std::string tot = format_token_count(snap.total_input + snap.total_output);
            const std::string label = trim_to_cells(tot, content_w);
            const int ver_w = cell_width(label);
            const int ver_x = block_x + block_w - 1 - ver_w;
            if (ver_x >= block_x + 1 && ver_w > 0) {
                draw_text(frame,
                          static_cast<std::uint32_t>(ver_x),
                          static_cast<std::uint32_t>(hint_y),
                          label,
                          sc.label,
                          bg);
            }
        }
    }
}

}  // namespace arbiter::opentui
