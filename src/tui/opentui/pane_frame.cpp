#include "tui/opentui/pane_frame.h"

#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <string>

namespace arbiter::opentui {

namespace {

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

std::string repeat_glyph(const std::string& glyph, int cells) {
    std::string out;
    if (cells <= 0 || glyph.empty()) return out;
    out.reserve(static_cast<size_t>(cells) * glyph.size());
    for (int i = 0; i < cells; ++i) out += glyph;
    return out;
}

} // namespace

void draw_pane_chrome(OpenTuiHandle frame, const TUI& tui) {
    const TuiChromeSnapshot chrome = tui.chrome_snapshot();
    const Rect& r = chrome.rect;
    if (r.w <= 0 || r.h <= 0) return;

    const TuiDesign& d = tui_design();
    const int pad = (r.w <= d.layout.dense_cols) ? 0 : d.layout.pane_padding_x;
    const int header_pad = std::max(0, std::min(d.layout.header_padding_x, std::max(0, r.w - 1)));
    const int status_inset = std::max(0, d.layout.status_inset_x);

    const std::uint32_t px = static_cast<std::uint32_t>(r.x);
    const std::uint32_t pw = static_cast<std::uint32_t>(r.w);

    fill_rect(frame, px, static_cast<std::uint32_t>(r.y), pw, static_cast<std::uint32_t>(r.h), d.bg.panel);

    const int identity_y = r.y;
    const int header_sep_y = r.y + 1;
    const int sep_y = r.y + r.h - TUI::kBottomPadRows - chrome.input_rows - TUI::kSepRows;
    const int input_top_y = sep_y + 1;
    const int input_bottom_y = r.y + r.h - TUI::kBottomPadRows - 1;
    const int hint_sep_y = r.y + r.h - 2;
    const int hint_y = r.y + r.h - 1;
    const int scroll_top_y = r.y + TUI::kHeaderRows;
    const int scroll_bottom_y = sep_y - 1;

    fill_rect(frame, px, static_cast<std::uint32_t>(identity_y), pw, 2, d.bg.header);

    const std::uint32_t content_x = px + static_cast<std::uint32_t>(pad);
    const int content_w = std::max(0, r.w - (pad * 2));

    std::string agent = d.component.agent_prefix + chrome.agent + d.component.agent_suffix;
    agent = trim_to_cells(agent, std::max(0, content_w / 2));

    const std::string& right_src = chrome.status_active ? chrome.status : chrome.stats;
    std::string right = right_src;
    if (!right.empty() && d.layout.status_pill) {
        right = d.component.status_prefix + right + d.component.status_suffix;
    }

    const int agent_cells = cell_width(agent);
    const int right_max = std::max(0, content_w - agent_cells - status_inset);
    right = trim_to_cells(right, right_max);
    const int right_cells = cell_width(right);
    const int title_max = std::max(0, content_w - agent_cells - right_cells - status_inset - 1);
    std::string title = trim_to_cells(chrome.title, title_max);

    std::uint32_t cx = content_x + static_cast<std::uint32_t>(header_pad);
    if (!agent.empty()) {
        draw_text(frame, cx, static_cast<std::uint32_t>(identity_y),
                  agent, d.text.inverse, d.accent.primary, 1);
        cx += static_cast<std::uint32_t>(agent_cells);
    }
    if (!title.empty()) {
        draw_text(frame, cx, static_cast<std::uint32_t>(identity_y), " ", d.text.muted, d.bg.header);
        cx += 1;
        draw_text(frame, cx, static_cast<std::uint32_t>(identity_y), title, d.text.muted, d.bg.header);
    }

    if (!right.empty()) {
        const int rx = r.x + r.w - pad - header_pad - right_cells;
        const TuiRgba& right_bg = d.layout.status_pill ? d.bg.status : d.bg.header;
        const TuiRgba& right_fg = chrome.status_active ? d.accent.info : d.text.subtle;
        if (rx >= r.x) {
            draw_text(frame,
                      static_cast<std::uint32_t>(rx),
                      static_cast<std::uint32_t>(identity_y),
                      right,
                      right_fg,
                      right_bg);
        }
    }

    const TuiRgba& header_line = chrome.focus_accent ? d.border.focus : d.border.subtle;
    const std::string header_rule = repeat_glyph(d.border.horizontal, r.w);
    if (!header_rule.empty()) {
        draw_text(frame, px, static_cast<std::uint32_t>(header_sep_y),
                  header_rule, header_line, d.bg.header);
    }

    if (scroll_top_y <= scroll_bottom_y) {
        fill_rect(frame,
                  px,
                  static_cast<std::uint32_t>(scroll_top_y),
                  pw,
                  static_cast<std::uint32_t>(scroll_bottom_y - scroll_top_y + 1),
                  d.bg.scroll);
    }

    fill_rect(frame, px, static_cast<std::uint32_t>(sep_y), pw, 1, d.bg.status);
    const std::string status_rule = repeat_glyph(d.border.horizontal, r.w);
    if (!status_rule.empty()) {
        draw_text(frame, px, static_cast<std::uint32_t>(sep_y),
                  status_rule, d.border.subtle, d.bg.status);
    }
    if (!chrome.pre_input_status.empty()) {
        std::string pre = " " + chrome.pre_input_status + " ";
        pre = trim_to_cells(pre, std::max(0, r.w - status_inset));
        draw_text(frame,
                  px + static_cast<std::uint32_t>(status_inset),
                  static_cast<std::uint32_t>(sep_y),
                  pre,
                  d.accent.info,
                  d.bg.status);
    }

    if (input_top_y <= input_bottom_y) {
        fill_rect(frame,
                  px,
                  static_cast<std::uint32_t>(input_top_y),
                  pw,
                  static_cast<std::uint32_t>(input_bottom_y - input_top_y + 1),
                  d.bg.input);
    }

    fill_rect(frame, px, static_cast<std::uint32_t>(hint_sep_y), pw, 2, d.bg.footer);
    const std::string footer_rule = repeat_glyph(d.border.horizontal, r.w);
    if (!footer_rule.empty()) {
        draw_text(frame, px, static_cast<std::uint32_t>(hint_sep_y),
                  footer_rule, d.border.subtle, d.bg.footer);
    }

    if (!chrome.footer_hint_visible || !d.layout.show_footer) return;

    const bool compact = r.w <= d.layout.compact_cols;
    std::string left = compact ? d.component.footer_left_compact : d.component.footer_left;
    std::string footer_right = compact ? d.component.footer_right_compact : d.component.footer_right;
    left = trim_to_cells(left, content_w);
    footer_right = trim_to_cells(footer_right, content_w);

    const int left_vis = cell_width(left);
    const int right_vis = cell_width(footer_right);
    const bool show_left = left_vis > 0 && content_w >= left_vis;
    const bool show_right = right_vis > 0 && content_w >= left_vis + d.layout.footer_gap + right_vis;
    std::uint32_t hx = content_x + static_cast<std::uint32_t>(header_pad);
    if (show_left) {
        draw_text(frame, hx, static_cast<std::uint32_t>(hint_y), left, d.text.subtle, d.bg.footer);
        hx += static_cast<std::uint32_t>(left_vis);
    }
    if (show_right) {
        const int rx = r.x + r.w - pad - header_pad - right_vis;
        if (rx > static_cast<int>(hx)) {
            draw_text(frame,
                      static_cast<std::uint32_t>(rx),
                      static_cast<std::uint32_t>(hint_y),
                      footer_right,
                      d.text.subtle,
                      d.bg.footer);
        }
    }
}

} // namespace arbiter::opentui
