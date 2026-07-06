#include "tui/opentui/history_sidebar_frame.h"

#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter::opentui {

namespace {

constexpr std::uint32_t kAttrBold = 1u << 0;
constexpr int kEdgePad = 1;
constexpr int kRowHeight = 2;

int cell_width(std::string_view s) {
    int w = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++w;
    }
    return w;
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
    if (max_cells <= 0) return {};
    while (!s.empty() && cell_width(s) > max_cells) {
        s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
            s.pop_back();
        }
    }
    return s;
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

int list_top_y(const Rect& r, bool /*focused*/) {
    return r.y + 2; // section label row, then list
}

int scroll_bottom_y(const Rect& pane_rect, int pane_input_rows) {
    return pane_rect.y + pane_rect.h - TUI::kBottomPadRows - pane_input_rows - TUI::kSepRows;
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

    std::string line = std::string(title);
    if (active) line = "\u25cf " + line;

    draw_text(frame,
              static_cast<std::uint32_t>(x),
              static_cast<std::uint32_t>(y),
              trim_to_cells(line, w),
              title_fg,
              row_bg,
              (selected || active) ? kAttrBold : 0);

    if (!subtitle.empty()) {
        draw_text(frame,
                  static_cast<std::uint32_t>(x + 1),
                  static_cast<std::uint32_t>(y + 1),
                  trim_to_cells(std::string(subtitle), std::max(0, w - 1)),
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

} // namespace

int history_sidebar_visible_rows(const Rect& sidebar_rect,
                                 const Rect& pane_rect,
                                 int pane_input_rows,
                                 bool focused) {
    if (sidebar_rect.h <= 0 || pane_rect.h <= 0) return 1;
    const int top = list_top_y(sidebar_rect, focused);
    const int bottom = scroll_bottom_y(pane_rect, pane_input_rows);
    const int list_h = std::max(0, bottom - top + 1);
    return std::max(1, list_h / kRowHeight);
}

void draw_history_sidebar(OpenTuiHandle frame,
                          const HistorySidebarSnapshot& snap,
                          const Rect& r,
                          const Rect& pane_rect,
                          int pane_input_rows) {
    if (frame == 0 || r.w <= 0 || r.h <= 0) return;

    const Rect& pr = pane_rect;
    if (pr.h <= 0) return;

    const TuiDesign& d = tui_design();
    const TuiRgba sbg = tui_sidebar_bg(d);
    const SidebarColors sc = tui_sidebar_colors(d);
    const int header_pad = std::max(0, std::min(d.layout.header_padding_x, std::max(0, r.w - 1)));

    const int sidebar_top = r.y;
    const int panel_top_y = sidebar_top;
    const int sep_y = scroll_bottom_y(pr, pane_input_rows);
    const int input_bottom_y = pr.y + pr.h - TUI::kBottomPadRows - 1;
    const int hint_y = pr.y + pr.h - 2;
    if (input_bottom_y < panel_top_y) return;

    const std::uint32_t px = static_cast<std::uint32_t>(r.x);
    const int block_x = r.x + kEdgePad;
    const int block_w = std::max(1, r.w - kEdgePad);
    const int content_x = block_x + header_pad;
    const int content_w = std::max(1, block_w - (header_pad * 2));

    const int panel_bottom_y = pr.y + pr.h - 1;
    const int block_h = std::max(1, panel_bottom_y - panel_top_y + 1);

    fill_rect(frame,
              px,
              static_cast<std::uint32_t>(panel_top_y),
              1,
              static_cast<std::uint32_t>(block_h),
              d.bg.base);

    fill_rect(frame,
              static_cast<std::uint32_t>(block_x),
              static_cast<std::uint32_t>(panel_top_y),
              static_cast<std::uint32_t>(block_w),
              static_cast<std::uint32_t>(block_h),
              sbg);

    const std::string_view sidebar_hint = snap.focused
        ? "\u2191\u2193 select  enter"
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

    struct RowItem {
        std::string title;
        std::string subtitle;
        bool is_new = false;
        std::string conv_id;
    };
    std::vector<RowItem> rows;
    rows.push_back({"+ New conversation", {}, true, {}});
    for (const auto& e : snap.entries) {
        RowItem ri;
        ri.title = e.title.empty() ? "Untitled" : e.title;
        ri.subtitle = basename_hint(e.cwd);
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
                 bg);
        row_y += kRowHeight;
    }
}

} // namespace arbiter::opentui
