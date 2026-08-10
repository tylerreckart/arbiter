#include "tui/opentui/history_sidebar_frame.h"

#include "styled_text.h"
#include "tui/opentui/engine.h"
#include "tui/opentui/rounded_box.h"
#include "tui/sidebar_format.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter::opentui {

namespace {

constexpr std::uint32_t kAttrBold = 1u << 0;
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

// Hold at the start, crawl to the end, hold, repeat — so clipped active
// titles eventually reveal the full name without a busy ticker.
std::string marquee_title(std::string_view title, int max_cells) {
    if (max_cells <= 0) return {};
    const int full = static_cast<int>(arbiter::display_width(title));
    if (full <= max_cells) return std::string(title);

    using clock = std::chrono::steady_clock;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        clock::now().time_since_epoch())
                        .count();

    constexpr int kHoldStartMs = 2800;
    constexpr int kHoldEndMs = 1800;
    constexpr int kStepMs = 160;
    const int travel = full - max_cells;
    const int scroll_ms = std::max(1, travel) * kStepMs;
    const int cycle = kHoldStartMs + scroll_ms + kHoldEndMs;
    const int t = static_cast<int>(ms % cycle);

    int offset = 0;
    if (t >= kHoldStartMs && t < kHoldStartMs + scroll_ms) {
        offset = (t - kHoldStartMs) / kStepMs;
        if (offset > travel) offset = travel;
    } else if (t >= kHoldStartMs + scroll_ms) {
        offset = travel;
    }
    return arbiter::slice_display_cols(title, offset, max_cells);
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
    // Blank row above the box, top border/title, blank row inside, then list.
    return r.y + 3;
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
              int row_h,
              std::string_view title,
              std::string_view subtitle,
              bool selected,
              bool active,
              bool editing,
              std::string_view edit_text,
              bool confirming,
              bool rename_is_folder,
              bool creating_folder,
              bool delete_is_folder,
              bool section,
              int indent,
              const TuiRgba& bg) {
    // Selection › indents only the selected row. Nested rows keep the tree
    // pipe fixed under folder `[-]`, with › after the pipe: `│ › title`.
    // Active (open) conversation: accent text. Section headers match the
    // Session sidebar (accent + bold).
    const TuiRgba& title_fg = (selected || active) ? d.accent.primary
                            : section              ? d.accent.primary
                                                   : sc.body;
    const TuiRgba& sub_fg = sc.label;
    const TuiRgba& guide_fg = d.text.muted;
    constexpr int kMarkW = 2;          // "› "
    constexpr int kMinusCol = 1;       // '-' within "[-] " / pipe column
    constexpr int kNestedTextCol = 3;  // one cell after the pipe
    // Pipe stays at x+kMinusCol; caret/text begin at the nested text column
    // (or at x for top-level rows).
    const int base_x = indent > 0 ? x + kNestedTextCol : x;
    const int mark_x = base_x;
    const int text_x = selected ? base_x + kMarkW : base_x;
    const int text_w = std::max(1, w - (text_x - x));

    if (section) {
        // Full box width with ├/┤ so the rule meets the vertical borders.
        draw_box_divider_row(frame,
                             x,
                             y,
                             w,
                             d.text.muted,
                             bg,
                             title,
                             &d.accent.primary);
        return;
    }

    auto paint_guide = [&](int dy) {
        if (indent <= 0 || dy < 0 || dy >= row_h) return;
        draw_text(frame,
                  static_cast<std::uint32_t>(x + kMinusCol),
                  static_cast<std::uint32_t>(y + dy),
                  "\u2502",
                  guide_fg,
                  bg);
    };

    // Guide first so nested selection paints as `│ › …`, not `› │ …`.
    paint_guide(0);
    if (selected) {
        draw_text(frame,
                  static_cast<std::uint32_t>(mark_x),
                  static_cast<std::uint32_t>(y),
                  "\u203A ",
                  d.accent.primary,
                  bg,
                  kAttrBold);
    }

    if (editing) {
        // Editable name + caret on the title line; caption on the subtitle.
        const char* caption = creating_folder ? "New folder"
                            : rename_is_folder ? "Rename folder"
                                               : "Rename chat";
        std::string line = std::string(edit_text) + "\u2588";
        draw_text(frame,
                  static_cast<std::uint32_t>(text_x),
                  static_cast<std::uint32_t>(y),
                  trim_to_cells(std::move(line), text_w),
                  title_fg,
                  bg,
                  kAttrBold);
        if (row_h >= 2) {
            paint_guide(1);
            draw_text(frame,
                      static_cast<std::uint32_t>(text_x),
                      static_cast<std::uint32_t>(y + 1),
                      trim_to_cells(caption, text_w),
                      d.text.muted,
                      bg);
        }
        return;
    }

    {
        const std::string title_view = active
            ? marquee_title(title, text_w)
            : trim_to_cells(std::string(title), text_w);
        draw_text(frame,
                  static_cast<std::uint32_t>(text_x),
                  static_cast<std::uint32_t>(y),
                  title_view,
                  title_fg,
                  bg,
                  (selected || active) ? kAttrBold : 0);
    }

    if (row_h < 2) return;

    paint_guide(1);
    if (confirming) {
        const std::string prompt = delete_is_folder
            ? "Delete folder? y/n"
            : "Delete chat? y/n";
        draw_text(frame,
                  static_cast<std::uint32_t>(text_x),
                  static_cast<std::uint32_t>(y + 1),
                  trim_to_cells(prompt, text_w),
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
                || part == "^W" || part == "b" || part == "m" || part == "y/n"
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

struct OverlayItem {
    std::string shortcut;  // single letter, or empty
    std::string label;
    bool destructive = false;
    bool current = false;  // move picker: already in this folder
};

// Floating action / move panel below the selected row. Heading + shortcut
// column + labels; destructive items separated by a hairline rule.
void draw_overlay_panel(OpenTuiHandle frame,
                        const TuiDesign& d,
                        int x,
                        int y,
                        int w,
                        int max_bottom_y,
                        std::string_view heading,
                        const std::vector<OverlayItem>& items,
                        int selected_index) {
    if (items.empty() || w <= 0) return;

    // Layout: heading, optional spacer before destructive cluster, items.
    int destructive_start = -1;
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].destructive) {
            destructive_start = static_cast<int>(i);
            break;
        }
    }
    const int sep_rows = (destructive_start > 0) ? 1 : 0;
    const int needed = 1 + static_cast<int>(items.size()) + sep_rows;
    const int h = std::min(needed, std::max(0, max_bottom_y - y + 1));
    if (h <= 0) return;

    const TuiRgba& surface = d.bg.header;
    fill_rect(frame,
              static_cast<std::uint32_t>(x),
              static_cast<std::uint32_t>(y),
              static_cast<std::uint32_t>(w),
              static_cast<std::uint32_t>(h),
              surface);

    const int text_w = std::max(1, w - 1);
    int row = 0;
    auto paint = [&](std::string line, const TuiRgba& fg, bool bold) {
        if (row >= h) return;
        draw_text(frame,
                  static_cast<std::uint32_t>(x + 1),
                  static_cast<std::uint32_t>(y + row),
                  trim_to_cells(std::move(line), text_w),
                  fg,
                  surface,
                  bold ? kAttrBold : 0);
        ++row;
    };

    paint(std::string(heading), d.text.muted, true);

    for (size_t i = 0; i < items.size(); ++i) {
        if (row >= h) break;
        if (destructive_start >= 0
            && static_cast<int>(i) == destructive_start
            && sep_rows > 0) {
            // Dim rule before destructive actions.
            paint(std::string(std::max(1, text_w), '-'), d.text.muted, false);
            if (row >= h) break;
        }
        const auto& it = items[i];
        const bool selected = static_cast<int>(i) == selected_index;
        // Selection is a leading marker only — no row background fill.
        std::string line;
        line += selected ? "\u203A" : " ";
        line += " ";
        if (!it.shortcut.empty()) {
            line += it.shortcut;
            line += " ";
        }
        line += it.label;
        if (it.current) line += " \u2713";

        const TuiRgba& fg = it.destructive ? d.accent.error
                          : selected       ? d.accent.primary
                                           : d.text.primary;
        paint(std::move(line), fg, selected);
    }
}

std::string_view focused_hint(const HistorySidebarSnapshot& snap) {
    if (snap.renaming) return "enter save  esc cancel";
    if (snap.confirming_delete) return "y/n confirm  esc cancel";
    if (snap.moving) return "\u2191\u2193 pick  enter  esc";
    if (snap.menu_open) return "\u2191\u2193  enter  esc";
    return "\u2191\u2193 select  m  f folder";
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
    // Most rows are now single-line (New / sections / folders); conversations
    // still take two. Use line budget as the page size so PgUp/PgDn and
    // scroll clamping stay useful with mixed heights.
    return std::max(1, list_h);
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

    // Focused hint must stay ≤ ~24 cells (box width minus padding) or
    // trim_to_cells drops the trailing "f folder" — PTY tests key on "fold".
    const std::string_view sidebar_hint = snap.focused
        ? focused_hint(snap)
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

    const bool only_new = snap.rows.size() <= 1;
    if (only_new) {
        draw_text(frame,
                  static_cast<std::uint32_t>(content_x),
                  static_cast<std::uint32_t>(y + 1),
                  trim_to_cells("No conversations yet", content_w),
                  d.text.muted,
                  bg);
    }

    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const int total = static_cast<int>(snap.rows.size());
    const int scroll = std::max(0, std::min(snap.scroll_offset, std::max(0, total - 1)));

    int row_y = y;
    for (int i = scroll; i < total; ++i) {
        const auto& row = snap.rows[static_cast<size_t>(i)];
        const bool is_section = row.kind == HistorySidebarRowKind::Section;
        const bool selected = snap.focused && (i == snap.selected) && !is_section;
        const bool active = row.kind == HistorySidebarRowKind::Conversation
            && row.id == snap.active_id;
        // New-folder naming uses a floating modal — keep the nav row intact.
        const bool editing = selected && snap.renaming && !snap.creating_folder;
        const bool confirming = selected && snap.confirming_delete;

        const int gap = history_sidebar_gap_before(row.kind, i);
        row_y += gap;

        // Keep subtitle while confirming (prompt replaces it). Collapse to
        // the title line when a panel will paint below so selection does
        // not leave an empty accent band under the title.
        const bool panel_open = selected && (snap.menu_open || snap.moving);

        int row_h = history_sidebar_row_height(row.kind);
        if (editing || confirming) row_h = 2;
        else if (panel_open) row_h = 1;
        if (row_y + row_h - 1 > sep_y) break;

        std::string title;
        std::string subtitle;
        if (row.kind == HistorySidebarRowKind::Section) {
            title = row.title;
        } else if (row.kind == HistorySidebarRowKind::Folder) {
            title = (row.expanded ? "[-] " : "[+] ") + row.title;
            if (confirming && snap.delete_is_folder) {
                subtitle = "Chats stay · unfiled";
            }
        } else if (row.kind == HistorySidebarRowKind::New) {
            title = row.title;
        } else {
            title = row.title;
            subtitle = relative_time(row.updated_at, now);
            if (row.total_tokens > 0) {
                subtitle += " · " + format_token_count(row.total_tokens)
                    + (row.total_tokens == 1 ? " tok" : " toks");
            }
            // Show the bound project dirname so directory-scoped chats are
            // obvious when switching across workspaces.
            if (!row.cwd.empty() && row.cwd.rfind("session:", 0) != 0) {
                std::string leaf = row.cwd;
                while (!leaf.empty() && (leaf.back() == '/' || leaf.back() == '\\'))
                    leaf.pop_back();
                const auto slash = leaf.find_last_of("/\\");
                if (slash != std::string::npos) leaf = leaf.substr(slash + 1);
                if (!leaf.empty()) subtitle += " · " + leaf;
            }
        }

        // Section dividers span the full box so ├/┤ meet the side borders;
        // other rows stay inset in the content column.
        const int paint_x = is_section ? block_x : content_x;
        const int paint_w = is_section ? block_w : content_w;
        draw_row(frame,
                 d,
                 sc,
                 paint_x,
                 row_y,
                 paint_w,
                 row_h,
                 title,
                 (panel_open && !confirming && !editing)
                     ? std::string_view{}
                     : std::string_view{subtitle},
                 selected,
                 active,
                 editing,
                 snap.rename_buffer,
                 confirming,
                 snap.rename_is_folder,
                 snap.creating_folder,
                 snap.delete_is_folder,
                 is_section,
                 row.indent,
                 bg);
        row_y += row_h;
    }
}

void draw_history_new_folder_modal(OpenTuiHandle frame,
                                   const HistorySidebarSnapshot& snap,
                                   const TUI& tui) {
    if (frame == 0 || !snap.creating_folder || !snap.renaming) return;

    const TuiDesign& d = tui_menu_design();
    const int cols = std::max(1, tui.cols());
    const int px = tui.left_col() - 1;
    const int input_top = tui.input_top_row_pub();

    constexpr int kH = 3;  // title, input, hint
    const int top = std::max(1, input_top - kH - 1);
    const int w = std::min({cols - 2, 40, cols});
    if (w < 12) return;
    const int x = px + 1;

    const TuiRgba& surface = d.bg.header;
    fill_rect(frame,
              static_cast<std::uint32_t>(x),
              static_cast<std::uint32_t>(top),
              static_cast<std::uint32_t>(w),
              static_cast<std::uint32_t>(kH),
              surface);

    const int text_w = std::max(1, w - 2);
    draw_text(frame,
              static_cast<std::uint32_t>(x + 1),
              static_cast<std::uint32_t>(top),
              trim_to_cells("New folder", text_w),
              d.accent.primary,
              surface,
              kAttrBold);

    std::string field = snap.rename_buffer + "\u2588";
    draw_text(frame,
              static_cast<std::uint32_t>(x + 1),
              static_cast<std::uint32_t>(top + 1),
              trim_to_cells(std::move(field), text_w),
              d.text.primary,
              surface,
              kAttrBold);

    draw_hint_text(frame,
                   static_cast<std::uint32_t>(x + 1),
                   static_cast<std::uint32_t>(top + 2),
                   text_w,
                   "enter save  esc cancel",
                   d,
                   surface);
}

void draw_history_sidebar_menu(OpenTuiHandle frame,
                               const HistorySidebarSnapshot& snap,
                               const Rect& r,
                               const Rect& pane_rect,
                               int pane_input_rows,
                               int pane_bottom_pad_rows) {
    if (frame == 0 || r.w <= 0 || r.h <= 0) return;
    if (!snap.focused || (!snap.menu_open && !snap.moving)) return;
    const Rect& pr = pane_rect;
    if (pr.h <= 0) return;

    const TuiDesign& d = tui_menu_design();
    const int bottom_pad = std::max(1, pane_bottom_pad_rows);
    const int box_bottom = pr.y + pr.h - bottom_pad - pane_input_rows;
    const int sep_y = box_bottom - 1;
    const int y = list_top_y(r, snap.focused);
    const int content_x = r.x + 1 + kBoxPad;
    const int content_w = std::max(1, r.w - 2 - 2 * kBoxPad);

    const int total = static_cast<int>(snap.rows.size());
    const int scroll = std::max(0, std::min(snap.scroll_offset, std::max(0, total - 1)));
    int selected_row_bottom = -1;
    int row_y = y;
    for (int i = scroll; i < total; ++i) {
        const auto& row = snap.rows[static_cast<size_t>(i)];
        const bool is_section = row.kind == HistorySidebarRowKind::Section;
        const bool selected = (i == snap.selected) && !is_section;
        const int gap = history_sidebar_gap_before(row.kind, i);
        row_y += gap;
        int row_h = history_sidebar_row_height(row.kind);
        if (selected
            && ((snap.renaming && !snap.creating_folder) || snap.confirming_delete)) {
            row_h = 2;
        } else if (selected && (snap.menu_open || snap.moving)) {
            row_h = 1;  // title only; menu docks flush under it
        }
        if (row_y + row_h - 1 > sep_y) break;
        if (selected) selected_row_bottom = row_y + row_h;
        row_y += row_h;
    }
    if (selected_row_bottom < 0) return;

    std::vector<OverlayItem> items;
    std::string_view heading;
    int selected_idx = 0;
    if (snap.moving) {
        heading = "Move to\u2026";
        selected_idx = snap.move_index;
        for (size_t i = 0; i < snap.move_labels.size(); ++i) {
            OverlayItem it;
            it.label = snap.move_labels[i];
            if (i < snap.move_is_current.size())
                it.current = snap.move_is_current[i];
            items.push_back(std::move(it));
        }
    } else if (snap.menu_is_folder) {
        heading = "Folder";
        selected_idx = snap.menu_index;
        items = {
            {"r", "Rename", false, false},
            {"n", "New chat here", false, false},
            {"d", "Delete folder", true, false},
        };
    } else if (snap.menu_is_new) {
        heading = "New";
        selected_idx = snap.menu_index;
        items = {
            {"f", "New folder", false, false},
        };
    } else {
        heading = "Chat";
        selected_idx = snap.menu_index;
        items = {
            {"o", "Open", false, false},
            {"r", "Rename", false, false},
            {"v", "Move to\u2026", false, false},
            {"d", "Delete", true, false},
        };
    }

    draw_overlay_panel(frame,
                       d,
                       content_x,
                       selected_row_bottom,
                       content_w,
                       sep_y,
                       heading,
                       items,
                       selected_idx);
}

} // namespace arbiter::opentui
