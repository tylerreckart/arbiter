#include "tui/opentui/pane_frame.h"

#include "styled_text.h"
#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <string>

namespace arbiter::opentui {

namespace {

constexpr int kHeaderAccentCells = 1;
constexpr std::uint32_t kAttrBold = 1u << 0;

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

bool is_command_token(std::string_view token) {
    return token == "esc" || token == "pg" || token == "pgup/dn"
        || token == "^W" || token == "^W w/z/c"
        || (!token.empty() && (token.front() == '/' || token.front() == '^'));
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
    const int pad = tui_pane_edge_pad(r.w, d);
    const int header_pad = std::max(0, std::min(d.layout.header_padding_x, std::max(0, r.w - 1)));
    const int bottom_pad = std::max(1, chrome.bottom_pad_rows);
    const bool paint_footer = chrome.footer_hint_visible && d.layout.show_footer;

    const std::uint32_t px = static_cast<std::uint32_t>(r.x);
    const std::uint32_t pw = static_cast<std::uint32_t>(r.w);
    const std::uint32_t block_x = static_cast<std::uint32_t>(r.x + pad);
    const std::uint32_t block_w = static_cast<std::uint32_t>(std::max(1, r.w - (pad * 2)));
    const int content_w = static_cast<int>(block_w);

    fill_rect(frame, px, static_cast<std::uint32_t>(r.y), pw, static_cast<std::uint32_t>(r.h), d.bg.scroll);

    const int sep_y = r.y + r.h - bottom_pad - chrome.input_rows - TUI::kSepRows;
    const int input_top_y = sep_y + 1;
    const int input_bottom_y = r.y + r.h - bottom_pad - 1;
    const int hint_y = r.y + r.h - 2;
    const int scroll_top_y = r.y;
    const int scroll_bottom_y = sep_y - 1;

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
        std::string pre = trim_to_cells(chrome.pre_input_status,
                                        std::max(0, content_w - header_pad));
        draw_text(frame,
                  block_x + static_cast<std::uint32_t>(header_pad),
                  static_cast<std::uint32_t>(sep_y),
                  pre,
                  d.accent.info,
                  d.bg.scroll);
    } else if (chrome.status_active && !chrome.status.empty()) {
        std::string status = trim_to_cells(chrome.status,
                                           std::max(0, content_w - header_pad));
        draw_text(frame,
                  block_x + static_cast<std::uint32_t>(header_pad),
                  static_cast<std::uint32_t>(sep_y),
                  status,
                  d.accent.info,
                  d.bg.scroll);
    } else if (!chrome.activity_badge.empty()) {
        // Unfocused activity/completion indicator (#41) — right-aligned on
        // the mid-separator so it doesn't collide with a focused prompt.
        std::string badge = trim_to_cells(chrome.activity_badge,
                                          std::max(0, content_w - header_pad));
        const int badge_w = cell_width(badge);
        const int bx = r.x + pad + static_cast<int>(block_w) - header_pad - badge_w;
        if (bx >= r.x + pad) {
            const TuiRgba& color = (badge.find("✗") != std::string::npos)
                ? d.accent.error
                : (badge.find("●") != std::string::npos) ? d.accent.warning
                                                         : d.accent.success;
            draw_text(frame,
                      static_cast<std::uint32_t>(bx),
                      static_cast<std::uint32_t>(sep_y),
                      badge,
                      color,
                      d.bg.scroll);
        }
    }

    if (input_top_y <= input_bottom_y) {
        const int input_h = input_bottom_y - input_top_y + 1;
        fill_rect(frame,
                  block_x,
                  static_cast<std::uint32_t>(input_top_y),
                  block_w,
                  static_cast<std::uint32_t>(input_h),
                  d.bg.header);
        const int accent_w = std::min(kHeaderAccentCells, static_cast<int>(block_w));
        if (accent_w > 0) {
            fill_rect(frame,
                      block_x,
                      static_cast<std::uint32_t>(input_top_y),
                      static_cast<std::uint32_t>(accent_w),
                      static_cast<std::uint32_t>(input_h),
                      d.accent.primary);
        }
    }

    // Only reserve/paint the hint row when the footer is shown, or when
    // chrome_compact_rows is off (blank placeholder so input doesn't jump).
    if (paint_footer || bottom_pad >= TUI::kBottomPadRows) {
        fill_rect(frame, block_x, static_cast<std::uint32_t>(hint_y), block_w, 1, d.bg.scroll);
    }

    if (!paint_footer) return;

    const bool narrow = r.w <= d.layout.compact_cols;
    std::string left;
    std::string footer_right;
    if (chrome.footer_hint_mode == FooterHintMode::Compact) {
        // Multi-pane focused: degrade to chord-only hints (#47) instead of
        // hiding the row entirely.
        left = narrow ? "^W w/z/c" : "^W w focus  ^W z zoom  ^W c close";
        footer_right = narrow ? "" : "^W b conversations";
    } else {
        left = narrow ? d.component.footer_left_compact : d.component.footer_left;
        footer_right = narrow ? d.component.footer_right_compact
                              : d.component.footer_right;
    }
    left = trim_to_cells(left, content_w);
    footer_right = trim_to_cells(footer_right, content_w);

    const int left_vis = cell_width(left);
    const int right_vis = cell_width(footer_right);
    const bool show_left = left_vis > 0 && content_w >= left_vis;
    const bool show_right = right_vis > 0 && content_w >= left_vis + d.layout.footer_gap + right_vis;
    const std::uint32_t content_x = block_x;
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
