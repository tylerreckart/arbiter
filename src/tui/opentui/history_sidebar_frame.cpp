#include "tui/opentui/history_sidebar_frame.h"

#include "styled_text.h"
#include "tui/opentui/engine.h"
#include "tui/opentui/rounded_box.h"
#include "tui/sidebar_format.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter::opentui {

namespace {

constexpr std::uint32_t kAttrBold = 1u << 0;
constexpr int kRowHeight = 2;
constexpr int kBoxPad = 1;  // breathing room inside the border

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

void fill_rect(OpenTuiHandle frame,
               std::uint32_t x,
               std::uint32_t y,
               std::uint32_t w,
               std::uint32_t h,
               const TuiRgba& bg) {
    if (w == 0 || h == 0) return;
    bufferFillRect(frame, x, y, w, h, bg.data());
}

std::string trim_to_cells(std::string s, int max_cells) {
    return arbiter::trim_to_display_cols(std::move(s), max_cells);
}

// "now" / "5m ago" / "2h ago" / "3d ago" / "Jun 12" (calendar date once it's
// been more than a week, since "40d ago" stops being a useful at-a-glance
// unit).
std::string relative_time(std::int64_t updated_at, std::int64_t now) {
    const std::int64_t delta = std::max<std::int64_t>(0, now - updated_at);
    if (delta < 60) return "now";
    if (delta < 3600) return std::to_string(delta / 60) + "m ago";
    if (delta < 86400) return std::to_string(delta / 3600) + "h ago";
    if (delta < 7 * 86400) return std::to_string(delta / 86400) + "d ago";

    static constexpr const char* kMonths[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::time_t t = static_cast<std::time_t>(updated_at);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s %d", kMonths[tmv.tm_mon % 12], tmv.tm_mday);
    return buf;
}

int list_top_y(const Rect& r, bool /*focused*/, bool filter_line_visible) {
    // Blank row above the box, top border/title, blank row inside, then
    // optional filter and the list.
    return r.y + 3 + (filter_line_visible ? 1 : 0);
}

int scroll_bottom_y(const Rect& pane_rect, int pane_input_rows, int pane_bottom_pad_rows) {
    const int bottom_pad = std::max(1, pane_bottom_pad_rows);
    const int sep_top = pane_rect.y + pane_rect.h - bottom_pad - pane_input_rows
                      - TUI::kSepRows;
    return sep_top - 1;
}

void draw_row(OpenTuiHandle frame,
              const TuiDesign& d,
              const SidebarColors& sc,
              int x,
              int y,
              int w,
              std::string_view title,
              std::string_view subtitle,
              bool selected,
              bool active,
              bool editing,
              std::string_view edit_text,
              bool confirming,
              const TuiRgba& bg) {
    const TuiRgba& row_bg = selected ? d.accent.primary : bg;
    // Active (open) conversation: accent text, not a background fill.
    const TuiRgba& title_fg = selected ? d.text.inverse
                            : active   ? d.accent.primary
                                       : sc.body;
    const TuiRgba& sub_fg = selected ? d.text.inverse : sc.label;
    // Selection covers title + subtitle so inverse text stays readable.
    const TuiRgba& sub_bg = selected ? row_bg : bg;

    if (selected) {
        fill_rect(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(y),
                  static_cast<std::uint32_t>(w),
                  static_cast<std::uint32_t>(kRowHeight),
                  row_bg);
    }

    if (editing) {
        std::string line = std::string(edit_text) + "\u2588";
        draw_text(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(y),
                  trim_to_cells(line, w),
                  title_fg,
                  row_bg,
                  kAttrBold);
    } else {
        draw_text(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(y),
                  trim_to_cells(std::string(title), w),
                  title_fg,
                  row_bg,
                  (selected || active) ? kAttrBold : 0);
    }

    if (confirming) {
        draw_text(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(y + 1),
                  trim_to_cells("Delete? [y/N]", w),
                  d.accent.error,
                  sub_bg,
                  kAttrBold);
    } else if (!subtitle.empty()) {
        draw_text(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(y + 1),
                  trim_to_cells(std::string(subtitle), w),
                  sub_fg,
                  sub_bg);
    }
}

void draw_hint_text(OpenTuiHandle frame,
                    std::uint32_t x,
                    std::uint32_t y,
                    int max_cells,
                    std::string_view text,
                    const TuiDesign& d,
                    const TuiRgba& bg) {
    std::string trimmed = trim_to_cells(std::string(text), max_cells);
    size_t i = 0;
    std::uint32_t cx = x;
    while (i < trimmed.size()) {
        const size_t start = i;
        const bool space = trimmed[i] == ' ';
        while (i < trimmed.size() && ((trimmed[i] == ' ') == space)) ++i;

        const std::string_view part(trimmed.data() + start, i - start);
        const bool command = !space
            && (part == "esc" || part == "enter" || part == "pg" || part == "pgup/dn"
                || part == "^W" || part == "b" || part == "m"
                || part == "\u2191\u2193" || (!part.empty() && part.front() == '/'));
        draw_text(frame,
                  cx,
                  y,
                  part,
                  command ? d.text.primary : d.text.muted,
                  bg,
                  command ? kAttrBold : 0);
        cx += static_cast<std::uint32_t>(cell_width(part));
    }
}

void draw_action_menu(OpenTuiHandle frame,
                      const TuiDesign& d,
                      int x,
                      int y,
                      int w,
                      int menu_index,
                      int max_bottom_y) {
    static constexpr const char* kLabels[] = {"Open", "Rename", "Delete"};
    static constexpr int kCount = 3;
    const int h = std::min(kCount, std::max(0, max_bottom_y - y + 1));
    if (h <= 0 || w <= 0) return;

    fill_rect(frame,
              static_cast<std::uint32_t>(x),
              static_cast<std::uint32_t>(y),
              static_cast<std::uint32_t>(w),
              static_cast<std::uint32_t>(h),
              d.bg.header);

    for (int i = 0; i < h; ++i) {
        const bool selected = i == menu_index;
        std::string line = selected ? "\u203A " : "  ";
        line += kLabels[i];
        const TuiRgba& fg = selected ? d.text.inverse
                          : (i == 2 ? d.accent.error : d.text.primary);
        const TuiRgba& bg = selected ? d.accent.primary : d.bg.header;
        draw_text(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(y + i),
                  trim_to_cells(std::move(line), w),
                  fg,
                  bg,
                  selected ? kAttrBold : 0);
    }
}

} // namespace

int history_sidebar_visible_rows(const Rect& sidebar_rect,
                                 const Rect& pane_rect,
                                 int pane_input_rows,
                                 bool focused,
                                 bool filter_line_visible,
                                 int pane_bottom_pad_rows) {
    if (sidebar_rect.h <= 0 || pane_rect.h <= 0) return 1;
    const int top = list_top_y(sidebar_rect, focused, filter_line_visible);
    const int bottom = scroll_bottom_y(pane_rect, pane_input_rows, pane_bottom_pad_rows);
    const int list_h = std::max(0, bottom - top + 1);
    return std::max(1, list_h / kRowHeight);
}

void draw_history_sidebar(OpenTuiHandle frame,
                          const HistorySidebarSnapshot& snap,
                          const Rect& r,
                          const Rect& pane_rect,
                          int pane_input_rows,
                          int pane_bottom_pad_rows) {
    if (frame == 0 || r.w <= 0 || r.h <= 0) return;

    const Rect& pr = pane_rect;
    if (pr.h <= 0) return;

    const TuiDesign& d = tui_design();
    const SidebarColors sc = tui_sidebar_colors(d);
    const int bottom_pad = std::max(1, pane_bottom_pad_rows);

    // One blank row above the box so it floats like the input strip; bottom
    // flush with the input box's bottom border (not the pane/footer edge).
    const int panel_top_y = r.y + 1;
    const int sep_y = scroll_bottom_y(pr, pane_input_rows, bottom_pad);
    const int input_bottom_y = pr.y + pr.h - bottom_pad - 1;
    const int hint_y = pr.y + pr.h - 2;
    if (input_bottom_y < panel_top_y + 1) return;

    const int block_x = r.x;
    const int block_w = r.w;
    const int content_x = block_x + 1 + kBoxPad;
    const int content_w = std::max(1, block_w - 2 - (kBoxPad * 2));
    const int block_h = std::max(2, input_bottom_y - panel_top_y + 1);

    const TuiRgba& bg = d.bg.scroll;
    const TuiRgba& border_fg = d.text.muted;
    draw_rounded_box(frame,
                     block_x,
                     panel_top_y,
                     block_w,
                     block_h,
                     border_fg,
                     bg,
                     "Conversations",
                     &d.accent.primary);

    const std::string_view sidebar_hint = snap.focused
        ? (snap.filtering ? "type to filter  esc clear"
                          : snap.menu_open ? "\u2191\u2193 move  enter  esc"
                                           : "\u2191\u2193 select  m menu  / filter")
        : "^W b focus";
    // Align with the pane's footer hints below the input box, not with the
    // sidebar border itself.
    if (hint_y > input_bottom_y) {
        draw_hint_text(frame,
                       static_cast<std::uint32_t>(block_x + 1),
                       static_cast<std::uint32_t>(hint_y),
                       std::max(0, block_w - 2),
                       sidebar_hint,
                       d,
                       bg);
    }

    // Leave one blank row directly beneath the title-bearing top border.
    int y = panel_top_y + 2;

    // Filter line: shown while typing ('/') and as long as a committed
    // filter is still narrowing the list.
    if (snap.filtering || !snap.filter.empty()) {
        std::string line = "/" + snap.filter;
        if (snap.filtering) line += "▏";   // caret while editing
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x),
                  static_cast<std::uint32_t>(y),
                  trim_to_cells(line, content_w),
                  snap.filtering ? d.accent.primary : d.text.muted,
                  bg);
        y += 1;
    }

    if (snap.entries.empty()) {
        const bool filtered = snap.filtering || !snap.filter.empty();
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x),
                  static_cast<std::uint32_t>(y + 1),
                  trim_to_cells(filtered ? "No matches" : "No conversations yet",
                                content_w),
                  d.text.muted,
                  bg);
    }

    struct RowItem {
        std::string title;
        std::string subtitle;
        bool is_new = false;
        std::string conv_id;
    };
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    std::vector<RowItem> rows;
    rows.push_back({"+ New conversation", {}, true, {}});
    for (const auto& e : snap.entries) {
        RowItem ri;
        ri.title = e.title.empty() ? "Untitled" : e.title;
        ri.subtitle = relative_time(e.updated_at, now);
        if (e.total_tokens > 0) {
            ri.subtitle += " · " + format_token_count(e.total_tokens)
                + (e.total_tokens == 1 ? " tok" : " toks");
        }
        ri.conv_id = e.id;
        rows.push_back(std::move(ri));
    }

    const int total = static_cast<int>(rows.size());
    const int scroll = std::max(0, std::min(snap.scroll_offset, std::max(0, total - 1)));

    int selected_row_y = -1;
    int row_y = y;
    for (int i = scroll; i < total && row_y + kRowHeight - 1 <= sep_y; ++i) {
        const bool selected = snap.focused && (i == snap.selected);
        const bool active = !rows[static_cast<size_t>(i)].is_new
            && rows[static_cast<size_t>(i)].conv_id == snap.active_id;
        const bool editing = selected && snap.renaming;
        const bool confirming = selected && snap.confirming_delete;
        if (selected) selected_row_y = row_y;
        // While the action menu is open, keep the title visible but skip the
        // subtitle — the overlay paints Open/Rename/Delete over that space.
        const bool menu_here = selected && snap.menu_open;
        draw_row(frame,
                 d,
                 sc,
                 content_x,
                 row_y,
                 content_w,
                 rows[static_cast<size_t>(i)].title,
                 menu_here ? std::string_view{} : rows[static_cast<size_t>(i)].subtitle,
                 selected,
                 active,
                 editing,
                 snap.rename_buffer,
                 confirming,
                 bg);
        row_y += kRowHeight;
    }

    if (snap.menu_open && snap.focused && selected_row_y >= 0) {
        // Anchor under the conversation title so Open/Rename/Delete read as
        // a menu on the active row; clamp so we never paint past the list.
        draw_action_menu(frame,
                         d,
                         content_x,
                         selected_row_y + 1,
                         content_w,
                         snap.menu_index,
                         sep_y);
    }
}

} // namespace arbiter::opentui
