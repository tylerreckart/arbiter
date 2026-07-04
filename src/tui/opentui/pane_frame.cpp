#include "tui/opentui/pane_frame.h"

#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <string>

namespace arbiter::opentui {

namespace {

constexpr int kHeaderAccentCells = 1;
constexpr std::uint32_t kAttrBold = 1u << 0;

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

bool is_command_token(std::string_view token) {
    return token == "esc" || token == "pg" || token == "pgup/dn"
        || (!token.empty() && token.front() == '/');
}

void draw_footer_hint(OpenTuiHandle frame,
                      std::uint32_t x,
                      std::uint32_t y,
                      std::string_view text,
                      const TuiDesign& d) {
    size_t i = 0;
    std::uint32_t cx = x;
    while (i < text.size()) {
        const size_t start = i;
        const bool space = text[i] == ' ';
        while (i < text.size() && ((text[i] == ' ') == space)) ++i;

        const std::string_view part(text.data() + start, i - start);
        const bool command = !space && is_command_token(part);
        draw_text(frame,
                  cx,
                  y,
                  part,
                  command ? d.text.primary : d.text.subtle,
                  d.bg.scroll,
                  command ? kAttrBold : 0);
        cx += static_cast<std::uint32_t>(cell_width(part));
    }
}

} // namespace

void draw_pane_chrome(OpenTuiHandle frame, const TUI& tui) {
    const TuiChromeSnapshot chrome = tui.chrome_snapshot();
    const Rect& r = chrome.rect;
    if (r.w <= 0 || r.h <= 0) return;

    const TuiDesign& d = tui_design();
    const int raw_pad = (r.w <= d.layout.dense_cols) ? 0 : std::max(0, d.layout.pane_padding_x);
    const int pad = std::min(raw_pad, std::max(0, (r.w - 1) / 2));
    const int header_pad = std::max(0, std::min(d.layout.header_padding_x, std::max(0, r.w - 1)));
    const int status_inset = std::max(0, d.layout.status_inset_x);

    const std::uint32_t px = static_cast<std::uint32_t>(r.x);
    const std::uint32_t pw = static_cast<std::uint32_t>(r.w);
    const std::uint32_t block_x = static_cast<std::uint32_t>(r.x + pad);
    const std::uint32_t block_w = static_cast<std::uint32_t>(std::max(1, r.w - (pad * 2)));

    fill_rect(frame, px, static_cast<std::uint32_t>(r.y), pw, static_cast<std::uint32_t>(r.h), d.bg.scroll);

    const int identity_y = r.y + 1;
    const int header_h = 3;
    const int header_text_y = identity_y + 1;
    const int sep_y = r.y + r.h - TUI::kBottomPadRows - chrome.input_rows - TUI::kSepRows;
    const int input_top_y = sep_y + 1;
    const int input_bottom_y = r.y + r.h - TUI::kBottomPadRows - 1;
    const int hint_y = r.y + r.h - 2;
    const int scroll_top_y = r.y + TUI::kHeaderRows;
    const int scroll_bottom_y = sep_y - 1;

    fill_rect(frame, block_x, static_cast<std::uint32_t>(identity_y), block_w, header_h, d.bg.header);
    const int accent_w = std::min(kHeaderAccentCells, static_cast<int>(block_w));
    if (accent_w > 0) {
        fill_rect(frame,
                  block_x,
                  static_cast<std::uint32_t>(identity_y),
                  static_cast<std::uint32_t>(accent_w),
                  header_h,
                  d.accent.primary);
    }

    const std::uint32_t content_x = block_x;
    const int content_w = static_cast<int>(block_w);

    std::string agent = d.component.agent_prefix + chrome.agent + d.component.agent_suffix;
    agent = trim_to_cells(agent, std::max(0, content_w / 2));

    const std::string& right_src = chrome.status_active ? chrome.status : chrome.stats;
    std::string right = right_src;
    if (!right.empty() && d.layout.status_pill) {
        right = d.component.status_prefix + right + d.component.status_suffix;
    }

    const int agent_cells = cell_width(agent);
    const int left_chrome_w = accent_w + header_pad;
    const int right_max = std::max(0, content_w - left_chrome_w - agent_cells - status_inset);
    right = trim_to_cells(right, right_max);
    const int right_cells = cell_width(right);
    const int title_max = std::max(0, content_w - left_chrome_w - agent_cells - right_cells - status_inset - 1);
    std::string title = trim_to_cells(chrome.title, title_max);

    std::uint32_t cx = content_x + static_cast<std::uint32_t>(left_chrome_w);
    if (!agent.empty()) {
        draw_text(frame, cx, static_cast<std::uint32_t>(header_text_y),
                  agent, d.text.primary, d.bg.header, 1);
        cx += static_cast<std::uint32_t>(agent_cells);
    }
    if (!title.empty()) {
        draw_text(frame, cx, static_cast<std::uint32_t>(header_text_y), " ", d.text.muted, d.bg.header);
        cx += 1;
        draw_text(frame, cx, static_cast<std::uint32_t>(header_text_y), title, d.text.muted, d.bg.header);
    }

    if (!right.empty()) {
        const int rx = r.x + pad + static_cast<int>(block_w) - header_pad - right_cells;
        const TuiRgba& right_bg = d.layout.status_pill ? d.bg.status : d.bg.header;
        const TuiRgba& right_fg = chrome.status_active ? d.accent.info : d.text.subtle;
        if (rx >= r.x + pad) {
            draw_text(frame,
                      static_cast<std::uint32_t>(rx),
                      static_cast<std::uint32_t>(header_text_y),
                      right,
                      right_fg,
                      right_bg);
        }
    }

    if (scroll_top_y <= scroll_bottom_y) {
        fill_rect(frame,
                  px,
                  static_cast<std::uint32_t>(scroll_top_y),
                  pw,
                  static_cast<std::uint32_t>(scroll_bottom_y - scroll_top_y + 1),
                  d.bg.scroll);
    }

    fill_rect(frame, block_x, static_cast<std::uint32_t>(sep_y), block_w, 1, d.bg.scroll);
    if (!chrome.pre_input_status.empty()) {
        std::string pre = " " + chrome.pre_input_status + " ";
        pre = trim_to_cells(pre, std::max(0, content_w - status_inset));
        draw_text(frame,
                  block_x + static_cast<std::uint32_t>(status_inset),
                  static_cast<std::uint32_t>(sep_y),
                  pre,
                  d.accent.info,
                  d.bg.scroll);
    }

    if (input_top_y <= input_bottom_y) {
        fill_rect(frame,
                  block_x,
                  static_cast<std::uint32_t>(input_top_y),
                  block_w,
                  static_cast<std::uint32_t>(input_bottom_y - input_top_y + 1),
                  d.bg.input);
    }

    fill_rect(frame, block_x, static_cast<std::uint32_t>(hint_y), block_w, 1, d.bg.scroll);

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
        draw_footer_hint(frame, hx, static_cast<std::uint32_t>(hint_y), left, d);
        hx += static_cast<std::uint32_t>(left_vis);
    }
    if (show_right) {
        const int rx = r.x + pad + static_cast<int>(block_w) - header_pad - right_vis;
        if (rx > static_cast<int>(hx)) {
            draw_footer_hint(frame,
                             static_cast<std::uint32_t>(rx),
                             static_cast<std::uint32_t>(hint_y),
                             footer_right,
                             d);
        }
    }
}

} // namespace arbiter::opentui
