#include "tui/opentui/history_sidebar_frame.h"

#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter::opentui {

namespace {

constexpr std::uint32_t kAttrBold = 1u << 0;
constexpr int kEdgePad = 1;
constexpr int kHeaderRows = 2;

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
    while (!s.empty() && cell_width(s) > max_cells) s.pop_back();
    return s;
}

std::string basename_hint(std::string_view path) {
    if (path.empty()) return {};
    const size_t pos = path.find_last_of('/');
    if (pos == std::string_view::npos) return std::string(path);
    return std::string(path.substr(pos + 1));
}

void draw_row(OpenTuiHandle frame,
              const TuiDesign& d,
              int x,
              int y,
              int w,
              std::string_view title,
              std::string_view subtitle,
              bool selected,
              bool active,
              const TuiRgba& bg) {
    const TuiRgba& title_fg = selected ? d.text.inverse : d.text.primary;
    const TuiRgba& row_bg = selected ? d.accent.primary : bg;
    const TuiRgba& sub_fg = selected ? d.text.inverse : d.text.subtle;

    if (selected) {
        fill_rect(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(y),
                  static_cast<std::uint32_t>(w),
                  1,
                  row_bg);
    }

    std::string line = std::string(title);
    if (active && !selected) line = "\u25cf " + line;
    else if (active) line = "\u25cf " + line;

    draw_text(frame,
              static_cast<std::uint32_t>(x),
              static_cast<std::uint32_t>(y),
              trim_to_cells(line, w),
              title_fg,
              row_bg,
              (selected || active) ? kAttrBold : 0);

    if (!subtitle.empty() && y + 1 < 10000) {
        draw_text(frame,
                  static_cast<std::uint32_t>(x + 1),
                  static_cast<std::uint32_t>(y + 1),
                  trim_to_cells(std::string(subtitle), std::max(0, w - 1)),
                  sub_fg,
                  bg);
    }
}

} // namespace

void draw_history_sidebar(OpenTuiHandle frame,
                          const HistorySidebarSnapshot& snap,
                          const Rect& r) {
    if (r.w <= 0 || r.h <= 0) return;

    const TuiDesign& d = tui_design();
    const TuiRgba& bg = d.bg.panel;
    const TuiRgba& header_bg = d.bg.header;

    fill_rect(frame,
              static_cast<std::uint32_t>(r.x),
              static_cast<std::uint32_t>(r.y),
              static_cast<std::uint32_t>(r.w),
              static_cast<std::uint32_t>(r.h),
              bg);

    const int content_x = r.x + kEdgePad;
    const int content_w = std::max(1, r.w - 2 * kEdgePad);
    int y = r.y + kEdgePad;

    draw_text(frame,
              static_cast<std::uint32_t>(content_x),
              static_cast<std::uint32_t>(y),
              trim_to_cells("Conversations", content_w),
              snap.focused ? d.accent.primary : d.text.muted,
              header_bg,
              kAttrBold);
    ++y;

    if (snap.focused) {
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x),
                  static_cast<std::uint32_t>(y),
                  trim_to_cells("\u2191\u2193 select  enter confirm  esc cancel",
                                content_w),
                  d.text.subtle,
                  bg);
    }
    y += kHeaderRows - 1;

    const int row_h = 2;
    const int list_top = y;
    const int list_h = std::max(0, r.y + r.h - list_top - kEdgePad);
    const int visible_rows = std::max(1, list_h / row_h);

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

    int row_y = list_top;
    for (int i = scroll; i < total && row_y + row_h <= r.y + r.h - kEdgePad; ++i) {
        const bool selected = (i == snap.selected);
        const bool active = !rows[static_cast<size_t>(i)].is_new
            && rows[static_cast<size_t>(i)].conv_id == snap.active_id;
        draw_row(frame,
                 d,
                 content_x,
                 row_y,
                 content_w,
                 rows[static_cast<size_t>(i)].title,
                 rows[static_cast<size_t>(i)].subtitle,
                 selected,
                 active,
                 bg);
        row_y += row_h;
    }

    if (snap.focused) {
        const TuiRgba& border = d.border.focus;
        const int bx = r.x + r.w - 1;
        for (int by = r.y; by < r.y + r.h; ++by) {
            draw_text(frame,
                      static_cast<std::uint32_t>(bx),
                      static_cast<std::uint32_t>(by),
                      d.border.vertical,
                      border,
                      bg);
        }
    }
}

} // namespace arbiter::opentui
