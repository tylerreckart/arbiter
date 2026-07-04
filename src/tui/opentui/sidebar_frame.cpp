#include "tui/opentui/sidebar_frame.h"

#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

namespace arbiter::opentui {

namespace {

constexpr std::uint32_t kAttrBold = 1u << 0;
constexpr int kEdgePad = 1;

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

std::string format_cost_usd(double usd) {
    if (usd <= 0.0) return "$0.00";
    if (usd < 0.01) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "$%.4f", usd);
        return buf;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "$%.2f", usd);
    return buf;
}

double estimate_cost_usd(int in_tokens, int out_tokens) {
    if (in_tokens <= 0 && out_tokens <= 0) return 0.0;
    return (static_cast<double>(in_tokens) / 1'000'000.0) * 3.0
         + (static_cast<double>(out_tokens) / 1'000'000.0) * 15.0;
}

std::string format_token_count(int n) {
    if (n < 0) n = 0;
    if (n >= 1'000'000) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1'000'000.0);
        return buf;
    }
    if (n >= 1'000) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(n) / 1'000.0);
        return buf;
    }
    return std::to_string(n);
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
              trim_to_cells(std::string(title), std::max(0, content_w)),
              d.accent.primary,
              bg,
              kAttrBold);
    return y + 1;
}

int draw_kv_line(OpenTuiHandle frame,
                 const TuiDesign& d,
                 int content_x,
                 int content_w,
                 int y,
                 std::string_view key,
                 std::string_view value,
                 const TuiRgba& value_fg,
                 const TuiRgba& bg) {
    const int key_cells = cell_width(key);
    const int gap = 1;
    const int value_max = std::max(0, content_w - key_cells - gap);
    std::string val = trim_to_cells(std::string(value), value_max);
    draw_text(frame,
              static_cast<std::uint32_t>(content_x),
              static_cast<std::uint32_t>(y),
              key,
              d.text.subtle,
              bg);
    draw_text(frame,
              static_cast<std::uint32_t>(content_x + key_cells + gap),
              static_cast<std::uint32_t>(y),
              val,
              value_fg,
              bg);
    return y + 1;
}

int draw_tool_list(OpenTuiHandle frame,
                   const TuiDesign& d,
                   int content_x,
                   int content_w,
                   int y,
                   int max_rows,
                   const std::vector<SidebarToolEntry>& entries,
                   std::string_view empty_label,
                   const TuiRgba& bg) {
    if (entries.empty()) {
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x),
                  static_cast<std::uint32_t>(y),
                  trim_to_cells(std::string(empty_label), std::max(0, content_w)),
                  d.text.subtle,
                  bg);
        return y + 1;
    }

    int row = y;
    const int shown = std::min(max_rows, static_cast<int>(entries.size()));
    for (int i = 0; i < shown; ++i) {
        const auto& e = entries[static_cast<size_t>(i)];
        const TuiRgba& mark_fg = e.ok ? d.accent.success : d.accent.error;
        const char* mark = e.ok ? "\u2713" : "\u2717";
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x),
                  static_cast<std::uint32_t>(row),
                  mark,
                  mark_fg,
                  bg);
        const int name_x = content_x + 2;
        const int name_w = std::max(0, content_w - 2);
        draw_text(frame,
                  static_cast<std::uint32_t>(name_x),
                  static_cast<std::uint32_t>(row),
                  trim_to_cells(e.name, name_w),
                  d.text.muted,
                  bg);
        ++row;
    }
    return row;
}

void draw_version_tag(OpenTuiHandle frame,
                      const TuiDesign& d,
                      int content_x,
                      int content_w,
                      int y) {
#ifdef INDEX_VERSION
    const char* version = INDEX_VERSION;
#else
    const char* version = "dev";
#endif
    std::string tag = "v";
    tag += version;
    tag = trim_to_cells(tag, std::max(0, content_w));
    const int tag_cells = cell_width(tag);
    const int tag_x = content_x + std::max(0, content_w - tag_cells);
    draw_text(frame,
              static_cast<std::uint32_t>(tag_x),
              static_cast<std::uint32_t>(y),
              tag,
              d.text.subtle,
              d.bg.header);
}

} // namespace

void draw_sidebar(OpenTuiHandle frame,
                  const SidebarSnapshot& snap,
                  const Rect& r,
                  const TuiChromeSnapshot& chrome) {
    if (frame == 0 || r.w <= 0 || r.h <= 0) return;

    const Rect& pr = chrome.rect;
    if (pr.h <= 0) return;

    const TuiDesign& d = tui_design();
    const int header_pad = std::max(0, std::min(d.layout.header_padding_x, std::max(0, r.w - 1)));

    const int identity_y = pr.y + 1;
    const int header_text_y = identity_y + 1;
    const int scroll_top_y = pr.y + TUI::kHeaderRows;
    const int sep_y = pr.y + pr.h - TUI::kBottomPadRows - chrome.input_rows - TUI::kSepRows;
    const int input_bottom_y = pr.y + pr.h - TUI::kBottomPadRows - 1;
    if (input_bottom_y < pr.y) return;

    const std::uint32_t px = static_cast<std::uint32_t>(r.x);
    const std::uint32_t pw = static_cast<std::uint32_t>(r.w);
    const int block_x = r.x;
    const int block_w = std::max(1, r.w - kEdgePad);
    const int content_x = block_x + header_pad;
    const int content_w = std::max(1, block_w - (header_pad * 2));

    const int edge_bottom_y = input_bottom_y + 1;
    const int edge_h = std::max(0, edge_bottom_y - pr.y + 1);

    fill_rect(frame, px, static_cast<std::uint32_t>(pr.y), pw, 1, d.bg.base);
    if (edge_h > 1) {
        fill_rect(frame,
                  static_cast<std::uint32_t>(r.x + r.w - 1),
                  static_cast<std::uint32_t>(pr.y),
                  1,
                  static_cast<std::uint32_t>(edge_h),
                  d.bg.base);
    }
    fill_rect(frame,
              px,
              static_cast<std::uint32_t>(edge_bottom_y),
              pw,
              1,
              d.bg.base);

    const int block_top_y = identity_y;
    const int block_h = std::max(1, input_bottom_y - block_top_y + 1);

    fill_rect(frame,
              static_cast<std::uint32_t>(block_x),
              static_cast<std::uint32_t>(block_top_y),
              static_cast<std::uint32_t>(block_w),
              static_cast<std::uint32_t>(block_h),
              d.bg.header);

    draw_text(frame,
              static_cast<std::uint32_t>(content_x),
              static_cast<std::uint32_t>(header_text_y),
              trim_to_cells("sidebar", content_w),
              d.text.primary,
              d.bg.header,
              kAttrBold);

    int y = scroll_top_y;
    const int scroll_bottom = sep_y;
    const TuiRgba& bg = d.bg.header;

    y = draw_section_label(frame, d, content_x, content_w, y, "usage", bg);
    const double cost = estimate_cost_usd(snap.total_input, snap.total_output);
    y = draw_kv_line(frame, d, content_x, content_w, y, "in",
                     format_token_count(snap.total_input), d.text.primary, bg);
    y = draw_kv_line(frame, d, content_x, content_w, y, "out",
                     format_token_count(snap.total_output), d.text.primary, bg);
    y = draw_kv_line(frame, d, content_x, content_w, y, "cost",
                     format_cost_usd(cost), d.text.muted, bg);
    y = draw_kv_line(frame, d, content_x, content_w, y, "turns",
                     std::to_string(snap.turn_count), d.text.muted, bg);
    if (!snap.last_model.empty()) {
        y = draw_kv_line(frame, d, content_x, content_w, y, "last",
                         trim_to_cells(snap.last_agent + " / " + snap.last_model, content_w - 6),
                         d.text.subtle, bg);
    }
    ++y;

    if (y <= scroll_bottom) {
        y = draw_section_label(frame, d, content_x, content_w, y, "task", bg);
        if (!snap.active_task.empty()) {
            std::string task = snap.active_task;
            int lines = 0;
            while (!task.empty() && lines < 3 && y <= scroll_bottom) {
                std::string line = trim_to_cells(task, content_w);
                draw_text(frame,
                          static_cast<std::uint32_t>(content_x),
                          static_cast<std::uint32_t>(y),
                          line,
                          d.text.primary,
                          bg);
                ++y;
                ++lines;
                if (line.size() >= task.size()) break;
                task.erase(0, line.size());
                while (!task.empty() && task.front() == ' ') task.erase(0, 1);
            }
        } else if (y <= scroll_bottom) {
            draw_text(frame,
                      static_cast<std::uint32_t>(content_x),
                      static_cast<std::uint32_t>(y),
                      trim_to_cells("(no active task)", content_w),
                      d.text.subtle,
                      bg);
            ++y;
        }
        ++y;
    }

    if (y <= scroll_bottom) {
        y = draw_section_label(frame, d, content_x, content_w, y, "tools", bg);
        if (snap.active_tool_calls > 0 && y <= scroll_bottom) {
            std::string live = std::to_string(snap.active_tool_calls) + " running\u2026";
            y = draw_kv_line(frame, d, content_x, content_w, y, "live", live,
                             d.accent.warning, bg);
        }
        const int tools_budget = std::max(1, scroll_bottom - y + 1);
        y = draw_tool_list(frame, d, content_x, content_w, y, tools_budget,
                           snap.tools, "(none yet)", bg);
        ++y;
    }

    if (y <= scroll_bottom) {
        y = draw_section_label(frame, d, content_x, content_w, y, "mcp", bg);
        const int mcp_budget = std::max(1, scroll_bottom - y + 1);
        draw_tool_list(frame, d, content_x, content_w, y, mcp_budget,
                       snap.mcp, "(none yet)", bg);
    }

    draw_version_tag(frame, d, content_x, content_w, input_bottom_y);
}

} // namespace arbiter::opentui
