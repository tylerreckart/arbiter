#include "tui/opentui/sidebar_frame.h"

#include "styled_text.h"
#include "tui/opentui/engine.h"
#include "tui/opentui/rounded_box.h"
#include "tui/sidebar_format.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>

namespace arbiter::opentui {

namespace {

constexpr std::uint32_t kAttrBold = 1u << 0;
constexpr int kBoxPad = 1;  // breathing room inside the border

std::string capitalize_label(std::string_view s) {
    if (s.empty()) return {};
    std::string out(s);
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

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

int draw_kv_line(OpenTuiHandle frame,
                 const SidebarColors& sc,
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
              sc.label,
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
                   const SidebarColors& sc,
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
                  sc.label,
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
                  sc.value,
                  bg);
        ++row;
    }
    return row;
}

int draw_todo_list(OpenTuiHandle frame,
                   const TuiDesign& d,
                   const SidebarColors& sc,
                   int content_x,
                   int content_w,
                   int y,
                   int max_rows,
                   const std::vector<SidebarTodoEntry>& entries,
                   const TuiRgba& bg) {
    if (entries.empty()) return y;
    int row = y;
    const int shown = std::min(max_rows, static_cast<int>(entries.size()));
    for (int i = 0; i < shown; ++i) {
        const auto& t = entries[static_cast<size_t>(i)];
        const TuiRgba& mark_fg = (t.status == "in_progress")
                                     ? d.accent.warning
                                     : sc.label;
        const char* mark = (t.status == "in_progress") ? "\u25b6" : "\u2022";
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x),
                  static_cast<std::uint32_t>(row),
                  mark,
                  mark_fg,
                  bg);
        std::string line = "#" + std::to_string(t.id) + " " + t.subject;
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x + 2),
                  static_cast<std::uint32_t>(row),
                  trim_to_cells(line, std::max(0, content_w - 2)),
                  sc.body,
                  bg);
        ++row;
    }
    return row;
}

int draw_schedule_list(OpenTuiHandle frame,
                       const TuiDesign& d,
                       const SidebarColors& sc,
                       int content_x,
                       int content_w,
                       int y,
                       int max_rows,
                       const std::vector<SidebarScheduleEntry>& entries,
                       const TuiRgba& bg) {
    if (entries.empty()) return y;
    int row = y;
    const int shown = std::min(max_rows, static_cast<int>(entries.size()));
    for (int i = 0; i < shown; ++i) {
        const auto& s = entries[static_cast<size_t>(i)];
        const TuiRgba& mark_fg = (s.status == "paused") ? sc.label : d.accent.primary;
        const char* mark = (s.status == "paused") ? "\u2016" : "\u23f1";
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x),
                  static_cast<std::uint32_t>(row),
                  mark,
                  mark_fg,
                  bg);
        std::string line = "#" + std::to_string(s.id) + " " + s.phrase;
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x + 2),
                  static_cast<std::uint32_t>(row),
                  trim_to_cells(line, std::max(0, content_w - 2)),
                  sc.value,
                  bg);
        ++row;
    }
    return row;
}

int draw_loop_list(OpenTuiHandle frame,
                   const TuiDesign& d,
                   const SidebarColors& sc,
                   int content_x,
                   int content_w,
                   int y,
                   int max_rows,
                   const std::vector<SidebarLoopEntry>& entries,
                   const TuiRgba& bg) {
    if (entries.empty()) return y;
    int row = y;
    const int shown = std::min(max_rows, static_cast<int>(entries.size()));
    for (int i = 0; i < shown; ++i) {
        const auto& l = entries[static_cast<size_t>(i)];
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x),
                  static_cast<std::uint32_t>(row),
                  "\u21bb",
                  d.accent.warning,
                  bg);
        std::string line = l.id + " " + l.agent_id + " i" + std::to_string(l.iter);
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x + 2),
                  static_cast<std::uint32_t>(row),
                  trim_to_cells(line, std::max(0, content_w - 2)),
                  sc.value,
                  bg);
        ++row;
    }
    return row;
}

} // namespace

void draw_sidebar(OpenTuiHandle frame,
                  const SidebarSnapshot& snap,
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

    // One blank row above the box; bottom flush with the input box bottom.
    const int panel_top_y = r.y + 1;
    const int sep_y = pr.y + pr.h - bottom_pad - pane_input_rows - TUI::kSepRows;
    const int input_bottom_y = pr.y + pr.h - bottom_pad - 1;
    if (input_bottom_y < panel_top_y + 1) return;

    const int block_x = r.x;
    const int block_w = r.w;
    const int content_x = block_x + 1 + kBoxPad;
    const int content_w = std::max(1, block_w - 2 - (kBoxPad * 2));
    const int block_h = std::max(2, input_bottom_y - panel_top_y + 1);

    const TuiRgba& bg = d.bg.scroll;
    draw_rounded_box(frame,
                     block_x,
                     panel_top_y,
                     block_w,
                     block_h,
                     d.text.muted,
                     bg,
                     "Session",
                     &d.accent.primary);

    // Leave one blank row directly beneath the title-bearing top border.
    int y = panel_top_y + 2;
    const int scroll_bottom = sep_y;

    y = draw_section_label(frame, d, content_x, content_w, y, "Context", bg);
    if (snap.context_pct_current >= 0) {
        std::string used = std::to_string(snap.context_pct_current) + "%";
        if (snap.last_context_tokens > 0 && snap.context_window > 0) {
            used += " " + format_token_count(snap.last_context_tokens)
                  + "/" + format_token_count(snap.context_window);
        }
        y = draw_kv_line(frame, sc, content_x, content_w, y, "used",
                         used, sc.body, bg);
        if (snap.context_pct_peak > snap.context_pct_current) {
            y = draw_kv_line(frame, sc, content_x, content_w, y, "peak",
                             std::to_string(snap.context_pct_peak) + "%",
                             sc.value, bg);
        }
    } else if (y <= scroll_bottom) {
        y = draw_kv_line(frame, sc, content_x, content_w, y, "used",
                         "\u2014", sc.label, bg);
    }
    std::string cost_line = format_cost_usd(snap.total_cost_usd);
    if (!snap.cost_basis.empty())
        cost_line += " (" + trim_to_cells(snap.cost_basis, 16) + ")";
    y = draw_kv_line(frame, sc, content_x, content_w, y, "in",
                     format_token_count(snap.total_input), sc.value, bg);
    y = draw_kv_line(frame, sc, content_x, content_w, y, "out",
                     format_token_count(snap.total_output), sc.value, bg);
    y = draw_kv_line(frame, sc, content_x, content_w, y, "cost",
                     cost_line, sc.value, bg);
    y = draw_kv_line(frame, sc, content_x, content_w, y, "turns",
                     std::to_string(snap.turn_count), sc.value, bg);
    ++y;

    if (y <= scroll_bottom) {
        y = draw_section_label(frame, d, content_x, content_w, y, "Agent", bg);
        const std::string agent = snap.focus_agent.empty() ? "(none)" : snap.focus_agent;
        y = draw_kv_line(frame, sc, content_x, content_w, y, "id",
                         agent, sc.body, bg);
        if (!snap.focus_model.empty()) {
            y = draw_kv_line(frame, sc, content_x, content_w, y, "model",
                             trim_to_cells(snap.focus_model, content_w - 7),
                             sc.value, bg);
        }
        if (!snap.last_model.empty() &&
            (snap.last_model != snap.focus_model || snap.last_agent != snap.focus_agent)) {
            y = draw_kv_line(frame, sc, content_x, content_w, y, "last",
                             trim_to_cells(snap.last_agent + " / " + snap.last_model,
                                           content_w - 6),
                             sc.value, bg);
        }
        ++y;
    }

    if (y <= scroll_bottom && !snap.todos.empty()) {
        y = draw_section_label(frame, d, content_x, content_w, y, "Todos", bg);
        const int budget = std::max(1, std::min(4, scroll_bottom - y + 1));
        y = draw_todo_list(frame, d, sc, content_x, content_w, y, budget, snap.todos, bg);
        ++y;
    }

    if (y <= scroll_bottom &&
        (!snap.schedules.empty() || !snap.loops.empty())) {
        y = draw_section_label(frame, d, content_x, content_w, y, "Scheduled", bg);
        int budget = std::max(1, scroll_bottom - y + 1);
        if (!snap.schedules.empty()) {
            const int sched_rows = std::min(budget, static_cast<int>(snap.schedules.size()));
            y = draw_schedule_list(frame, d, sc, content_x, content_w, y, sched_rows,
                                   snap.schedules, bg);
            budget = std::max(0, scroll_bottom - y + 1);
        }
        if (!snap.loops.empty() && budget > 0) {
            y = draw_loop_list(frame, d, sc, content_x, content_w, y, budget, snap.loops, bg);
        }
        ++y;
    }

    if (y <= scroll_bottom) {
        y = draw_section_label(frame, d, content_x, content_w, y, "Tools", bg);
        if (snap.active_tool_calls > 0 && y <= scroll_bottom) {
            std::string live = std::to_string(snap.active_tool_calls) + " running\u2026";
            y = draw_kv_line(frame, sc, content_x, content_w, y, "live", live,
                             d.accent.warning, bg);
        }
        const int tools_budget = std::max(1, scroll_bottom - y + 1);
        y = draw_tool_list(frame, d, sc, content_x, content_w, y, tools_budget,
                           snap.tools, "(none yet)", bg);
        ++y;
    }

    if (y <= scroll_bottom && !snap.mcp.empty()) {
        y = draw_section_label(frame, d, content_x, content_w, y, "MCP", bg);
        const int mcp_budget = std::max(1, scroll_bottom - y + 1);
        draw_tool_list(frame, d, sc, content_x, content_w, y, mcp_budget,
                       snap.mcp, "(none yet)", bg);
    }
}

} // namespace arbiter::opentui
