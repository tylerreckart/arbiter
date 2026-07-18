#include "tui/opentui/history_sidebar_frame.h"

#include "styled_text.h"
#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter::opentui {

namespace {

constexpr std::uint32_t kAttrBold = 1u << 0;
constexpr int kRowHeight = 2;

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

std::string capitalize_label(std::string_view s) {
    if (s.empty()) return {};
    std::string out(s);
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

int draw_section_label(OpenTuiHandle frame,
                       const TuiDesign& d,
                       int content_x,
                       int content_w,
                       int y,
                       std::string_view title,
                       const TuiRgba& bg) {
    draw_text(frame,
              static_cast<std::uint32_t>(content_x),
              static_cast<std::uint32_t>(y),
              trim_to_cells(capitalize_label(title), std::max(0, content_w)),
              d.accent.primary,
              bg,
              kAttrBold);
    return y + 1;
}

std::string basename_hint(std::string_view path) {
    if (path.empty()) return {};
    const size_t pos = path.find_last_of('/');
    if (pos == std::string_view::npos) return std::string(path);
    return std::string(path.substr(pos + 1));
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

int list_top_y(const Rect& r, bool /*focused*/) {
    return r.y + 2; // section label row, then list
}

int scroll_bottom_y(const Rect& pane_rect, int pane_input_rows, int pane_bottom_pad_rows) {
    const int bottom_pad = std::max(1, pane_bottom_pad_rows);
    return pane_rect.y + pane_rect.h - bottom_pad - pane_input_rows - TUI::kSepRows;
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
    const TuiRgba& title_fg = selected ? d.text.inverse : sc.body;
    const TuiRgba& sub_fg = selected ? d.text.inverse : sc.label;

    if (selected) {
        fill_rect(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(y),
                  static_cast<std::uint32_t>(w),
                  1,
                  row_bg);
    }

    // 1-col accent bar in the gutter marks the active conversation, leaving
    // every row's title starting at the same column regardless of state.
    constexpr int kGutterW = 1;
    if (active) {
        fill_rect(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(y),
                  static_cast<std::uint32_t>(kGutterW),
                  1,
                  d.accent.primary);
    }
    const int text_x = x + kGutterW;
    const int text_w = std::max(0, w - kGutterW);

    if (editing) {
        std::string line = std::string(edit_text) + "\u2588";
        draw_text(frame,
                  static_cast<std::uint32_t>(text_x),
                  static_cast<std::uint32_t>(y),
                  trim_to_cells(line, text_w),
                  title_fg,
                  row_bg,
                  kAttrBold);
    } else {
        draw_text(frame,
                  static_cast<std::uint32_t>(text_x),
                  static_cast<std::uint32_t>(y),
                  trim_to_cells(std::string(title), text_w),
                  title_fg,
                  row_bg,
                  (selected || active) ? kAttrBold : 0);
    }

    if (confirming) {
        draw_text(frame,
                  static_cast<std::uint32_t>(text_x),
                  static_cast<std::uint32_t>(y + 1),
                  trim_to_cells("Delete? [y/N]", text_w),
                  d.accent.error,
                  bg,
                  kAttrBold);
    } else if (!subtitle.empty()) {
        draw_text(frame,
                  static_cast<std::uint32_t>(text_x),
                  static_cast<std::uint32_t>(y + 1),
                  trim_to_cells(std::string(subtitle), text_w),
                  sub_fg,
                  bg);
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
                || part == "^W" || part == "b"
                || part == "\u2191\u2193" || (!part.empty() && part.front() == '/'));
        draw_text(frame,
                  cx,
                  y,
                  part,
                  command ? d.text.primary : d.text.subtle,
                  bg,
                  command ? kAttrBold : 0);
        cx += static_cast<std::uint32_t>(cell_width(part));
    }
}

// A dithered shade block, not a thin rule (d.border.vertical) or a solid
// block (too heavy) — a single-cell-wide glyph only covers part of the
// cell, so whichever side of the seam it lands on, the other side still
// reads as a gap. This still fills the whole column so it touches both
// neighbors, but at partial density so it doesn't read as a solid wall.
constexpr const char* kBorderGlyph = "▏";

void draw_vertical_border(OpenTuiHandle frame,
                          int x,
                          int y0,
                          int h,
                          const TuiRgba& fg,
                          const TuiRgba& bg) {
    for (int yy = y0; yy < y0 + h; ++yy) {
        draw_text(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(yy),
                  kBorderGlyph,
                  fg,
                  bg);
    }
}

// Pane content is inset from its own rect by pane_padding_x (see
// draw_pane_chrome); the border needs to sit in that gap, flush against
// the pane's real content edge, or it reads as floating in dead space.
int pane_edge_pad(const TuiDesign& d, int pane_w) {
    return tui_pane_edge_pad(pane_w, d);
}

} // namespace

int history_sidebar_visible_rows(const Rect& sidebar_rect,
                                 const Rect& pane_rect,
                                 int pane_input_rows,
                                 bool focused,
                                 int pane_bottom_pad_rows) {
    if (sidebar_rect.h <= 0 || pane_rect.h <= 0) return 1;
    const int top = list_top_y(sidebar_rect, focused);
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
    const TuiRgba sbg = tui_sidebar_bg(d);
    const SidebarColors sc = tui_sidebar_colors(d);
    const int header_pad = std::max(0, std::min(d.layout.header_padding_x, std::max(0, r.w - 1)));
    const int bottom_pad = std::max(1, pane_bottom_pad_rows);

    const int sidebar_top = r.y;
    const int panel_top_y = sidebar_top;
    const int sep_y = scroll_bottom_y(pr, pane_input_rows, bottom_pad);
    const int input_bottom_y = pr.y + pr.h - bottom_pad - 1;
    // Prefer the classic hint row when it exists; otherwise the trailing pad.
    const int hint_y = pr.y + pr.h - (bottom_pad >= TUI::kBottomPadRows ? 2 : 1);
    if (input_bottom_y < panel_top_y) return;

    const int block_x = r.x;
    const int block_w = r.w;
    const int content_x = block_x + header_pad;
    const int content_w = std::max(1, block_w - (header_pad * 2));

    const int panel_bottom_y = pr.y + pr.h - 1;
    const int block_h = std::max(1, panel_bottom_y - panel_top_y + 1);

    fill_rect(frame,
              static_cast<std::uint32_t>(block_x),
              static_cast<std::uint32_t>(panel_top_y),
              static_cast<std::uint32_t>(block_w),
              static_cast<std::uint32_t>(block_h),
              sbg);

    // Border sits flush against the pane's real content edge, inside its
    // own padding gap, so it touches the content area with no dead space.
    const int pane_pad = pane_edge_pad(d, pr.w);
    const int border_x = pr.x + std::max(0, pane_pad - 1);
    draw_vertical_border(frame, border_x, panel_top_y, block_h, d.bg.status, d.bg.scroll);

    const std::string_view sidebar_hint = snap.focused
        ? (snap.filtering ? "type to filter  esc clear"
                          : "\u2191\u2193 select  enter  / filter")
        : "^W b focus";
    draw_hint_text(frame,
                   static_cast<std::uint32_t>(content_x),
                   static_cast<std::uint32_t>(hint_y),
                   content_w,
                   sidebar_hint,
                   d,
                   sbg);

    const TuiRgba& bg = sbg;
    int y = panel_top_y + 1;
    y = draw_section_label(frame, d, content_x, content_w, y, "Conversations", bg);

    // Filter line: shown while typing ('/') and as long as a committed
    // filter is still narrowing the list.
    if (snap.filtering || !snap.filter.empty()) {
        std::string line = "/" + snap.filter;
        if (snap.filtering) line += "▏";   // caret while editing
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x),
                  static_cast<std::uint32_t>(y),
                  trim_to_cells(line, content_w),
                  snap.filtering ? d.accent.primary : d.text.subtle,
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
                  d.text.subtle,
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
        ri.subtitle = relative_time(e.updated_at, now) + " · " + basename_hint(e.cwd);
        ri.conv_id = e.id;
        rows.push_back(std::move(ri));
    }

    const int total = static_cast<int>(rows.size());
    const int scroll = std::max(0, std::min(snap.scroll_offset, std::max(0, total - 1)));

    int row_y = y;
    for (int i = scroll; i < total && row_y + kRowHeight - 1 <= sep_y; ++i) {
        const bool selected = snap.focused && (i == snap.selected);
        const bool active = !rows[static_cast<size_t>(i)].is_new
            && rows[static_cast<size_t>(i)].conv_id == snap.active_id;
        const bool editing = selected && snap.renaming;
        const bool confirming = selected && snap.confirming_delete;
        draw_row(frame,
                 d,
                 sc,
                 content_x,
                 row_y,
                 content_w,
                 rows[static_cast<size_t>(i)].title,
                 rows[static_cast<size_t>(i)].subtitle,
                 selected,
                 active,
                 editing,
                 snap.rename_buffer,
                 confirming,
                 bg);
        row_y += kRowHeight;
    }
}

} // namespace arbiter::opentui
