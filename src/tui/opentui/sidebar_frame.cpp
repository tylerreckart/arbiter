#include "tui/opentui/sidebar_frame.h"

#include "tui/opentui/engine.h"
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
constexpr int kEdgePad = 1;
// Blank row above the sidebar panel — matches the main pane scroll area (bg.scroll).
constexpr int kTopPadRows = 1;

std::string capitalize_label(std::string_view s) {
    if (s.empty()) return {};
    std::string out(s);
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

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

void draw_version_tag(OpenTuiHandle frame,
                      const SidebarColors& sc,
                      int content_x,
                      int content_w,
                      int y,
                      const TuiRgba& bg) {
#ifdef INDEX_VERSION
    const char* version = INDEX_VERSION;
#else
    const char* version = "dev";
#endif
    std::string tag = "Arbiter v";
    tag += version;
    tag = trim_to_cells(tag, std::max(0, content_w));
    const int tag_cells = cell_width(tag);
    const int tag_x = content_x + std::max(0, content_w - tag_cells);
    draw_text(frame,
              static_cast<std::uint32_t>(tag_x),
              static_cast<std::uint32_t>(y),
              tag,
              sc.label,
              bg);
}

} // namespace

void draw_sidebar(OpenTuiHandle frame,
                  const SidebarSnapshot& snap,
                  const Rect& r,
                  const Rect& pane_rect,
                  int pane_input_rows) {
    if (frame == 0 || r.w <= 0 || r.h <= 0) return;

    const Rect& pr = pane_rect;
    if (pr.h <= 0) return;

    const TuiDesign& d = tui_design();
    const SidebarColors sc = tui_sidebar_colors(d);
    const int header_pad = std::max(0, std::min(d.layout.header_padding_x, std::max(0, r.w - 1)));

    const int sidebar_top = r.y;
    const int panel_top_y = sidebar_top + kTopPadRows;
    const int sep_y = pr.y + pr.h - TUI::kBottomPadRows - pane_input_rows - TUI::kSepRows;
    const int input_bottom_y = pr.y + pr.h - TUI::kBottomPadRows - 1;
    const int hint_y = pr.y + pr.h - 2;
    if (input_bottom_y < panel_top_y) return;

    const std::uint32_t px = static_cast<std::uint32_t>(r.x);
    const std::uint32_t pw = static_cast<std::uint32_t>(r.w);
    const int block_x = r.x;
    const int block_w = std::max(1, r.w - kEdgePad);
    const int content_x = block_x + header_pad;
    const int content_w = std::max(1, block_w - (header_pad * 2));

    const int edge_bottom_y = input_bottom_y + 1;
    const int edge_h = std::max(0, edge_bottom_y - sidebar_top + 1);

    const int block_h = std::max(1, input_bottom_y - panel_top_y + 1);

    if (kTopPadRows > 0) {
        fill_rect(frame,
                  px,
                  static_cast<std::uint32_t>(sidebar_top),
                  pw,
                  static_cast<std::uint32_t>(kTopPadRows),
                  d.bg.scroll);
    }

    fill_rect(frame,
              static_cast<std::uint32_t>(block_x),
              static_cast<std::uint32_t>(panel_top_y),
              static_cast<std::uint32_t>(block_w),
              static_cast<std::uint32_t>(block_h),
              d.bg.header);

    if (edge_h > 0) {
        fill_rect(frame,
                  static_cast<std::uint32_t>(r.x + r.w - 1),
                  static_cast<std::uint32_t>(sidebar_top),
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
    fill_rect(frame,
              px,
              static_cast<std::uint32_t>(hint_y),
              pw,
              1,
              d.bg.scroll);
    if (hint_y + 1 < pr.y + pr.h) {
        fill_rect(frame,
                  px,
                  static_cast<std::uint32_t>(hint_y + 1),
                  pw,
                  1,
                  d.bg.base);
    }

    int y = panel_top_y + 1;
    const int scroll_bottom = sep_y;
    const TuiRgba& bg = d.bg.header;

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

    if (y <= scroll_bottom) {
        y = draw_section_label(frame, d, content_x, content_w, y, "Task", bg);
        if (!snap.active_task.empty()) {
            std::string task = snap.active_task;
            int lines = 0;
            while (!task.empty() && lines < 3 && y <= scroll_bottom) {
                std::string line = trim_to_cells(task, content_w);
                draw_text(frame,
                          static_cast<std::uint32_t>(content_x),
                          static_cast<std::uint32_t>(y),
                          line,
                          sc.body,
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
                      sc.label,
                      bg);
            ++y;
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

    if (y <= scroll_bottom) {
        y = draw_section_label(frame, d, content_x, content_w, y, "MCP", bg);
        const int mcp_budget = std::max(1, scroll_bottom - y + 1);
        draw_tool_list(frame, d, sc, content_x, content_w, y, mcp_budget,
                       snap.mcp, "(none yet)", bg);
    }

    draw_version_tag(frame, sc, content_x, content_w, hint_y, d.bg.scroll);
}

} // namespace arbiter::opentui
