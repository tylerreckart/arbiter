#include "tui/opentui/pane_frame.h"

#include "tui/opentui/engine.h"

#include <algorithm>
#include <string>

namespace arbiter::opentui {

namespace {

using Rgba = std::array<std::uint16_t, 4>;

Rgba rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return rgba8(r, g, b);
}

// Region backgrounds — line borders appear only on multi-pane splits (layout pass).
Rgba pane_bg()    { return rgb(0x1e, 0x1e, 0x2e); }
Rgba header_bg()  { return rgb(0x26, 0x26, 0x36); }
Rgba scroll_bg()  { return rgb(0x18, 0x18, 0x25); }
Rgba status_bg()  { return rgb(0x22, 0x22, 0x32); }
Rgba input_bg()   { return rgb(0x2a, 0x2a, 0x3c); }
Rgba footer_bg()  { return rgb(0x1c, 0x1c, 0x2a); }

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
               const Rgba& fg,
               const Rgba& bg) {
    if (text.empty()) return;
    bufferDrawText(frame,
                   text.data(),
                   static_cast<std::uint32_t>(text.size()),
                   x,
                   y,
                   fg.data(),
                   bg.data(),
                   0);
}

void fill_rect(OpenTuiHandle frame,
               std::uint32_t x,
               std::uint32_t y,
               std::uint32_t w,
               std::uint32_t h,
               const Rgba& bg) {
    if (w == 0 || h == 0) return;
    bufferFillRect(frame, x, y, w, h, bg.data());
}

} // namespace

void draw_pane_chrome(OpenTuiHandle frame, const TUI& tui) {
    const TuiChromeSnapshot chrome = tui.chrome_snapshot();
    const Rect& r = chrome.rect;
    if (r.w <= 0 || r.h <= 0) return;

    const Rgba kPane   = pane_bg();
    const Rgba kHeader = header_bg();
    const Rgba kScroll = scroll_bg();
    const Rgba kStatus = status_bg();
    const Rgba kInput  = input_bg();
    const Rgba kFooter = footer_bg();

    const Rgba fg  = rgb(0xab, 0xb2, 0xbf);
    const Rgba dim = rgb(0x5c, 0x63, 0x70);

    const std::uint32_t px = static_cast<std::uint32_t>(r.x);
    const std::uint32_t pw = static_cast<std::uint32_t>(r.w);

    fill_rect(frame, px, static_cast<std::uint32_t>(r.y), pw, static_cast<std::uint32_t>(r.h), kPane);

    const int identity_y = r.y;
    const int sep_y = r.y + r.h - TUI::kBottomPadRows - chrome.input_rows - TUI::kSepRows;
    const int input_top_y = sep_y + 1;
    const int input_bottom_y = r.y + r.h - TUI::kBottomPadRows - 1;
    const int hint_sep_y = r.y + r.h - 2;
    const int hint_y = r.y + r.h - 1;
    const int scroll_top_y = r.y + TUI::kHeaderRows;
    const int scroll_bottom_y = sep_y - 1;

    // Header block (identity row + row below).
    fill_rect(frame, px, static_cast<std::uint32_t>(identity_y), pw, 2, kHeader);

    std::string agent = chrome.agent;
    if (cell_width(agent) > r.w) agent.resize(static_cast<size_t>(std::max(0, r.w)));

    int left_cells = cell_width(agent);
    std::string title;
    if (!chrome.title.empty() && left_cells + 1 + cell_width(chrome.title) <= r.w) {
        title = chrome.title;
        left_cells += 1 + cell_width(title);
    }

    const std::string& right_src =
        chrome.status_active ? chrome.status : chrome.stats;
    std::string right = right_src;
    const int avail = std::max(0, r.w - left_cells - 2);
    if (cell_width(right) > avail) {
        right.resize(static_cast<size_t>(avail));
    }
    const int right_cells = cell_width(right);
    const int pad = std::max(0, avail - right_cells);

    draw_text(frame, px, static_cast<std::uint32_t>(identity_y), agent, fg, kHeader);
    std::uint32_t cx = px + static_cast<std::uint32_t>(cell_width(agent));
    if (!title.empty()) {
        draw_text(frame, cx, static_cast<std::uint32_t>(identity_y), " ", fg, kHeader);
        cx += 1;
        draw_text(frame, cx, static_cast<std::uint32_t>(identity_y), title, dim, kHeader);
        cx += static_cast<std::uint32_t>(cell_width(title));
    }
    cx += static_cast<std::uint32_t>(2 + pad);
    if (!right.empty()) {
        draw_text(frame, cx, static_cast<std::uint32_t>(identity_y), right, dim, kHeader);
    }

    // Scrollback content area.
    if (scroll_top_y <= scroll_bottom_y) {
        fill_rect(frame,
                  px,
                  static_cast<std::uint32_t>(scroll_top_y),
                  pw,
                  static_cast<std::uint32_t>(scroll_bottom_y - scroll_top_y + 1),
                  kScroll);
    }

    // Tool-call / pre-input status row.
    fill_rect(frame, px, static_cast<std::uint32_t>(sep_y), pw, 1, kStatus);
    if (!chrome.pre_input_status.empty()) {
        draw_text(frame,
                  px + 2,
                  static_cast<std::uint32_t>(sep_y),
                  chrome.pre_input_status,
                  dim,
                  kStatus);
    }

    // Input area.
    if (input_top_y <= input_bottom_y) {
        fill_rect(frame,
                  px,
                  static_cast<std::uint32_t>(input_top_y),
                  pw,
                  static_cast<std::uint32_t>(input_bottom_y - input_top_y + 1),
                  kInput);
    }

    // Footer rows.
    fill_rect(frame, px, static_cast<std::uint32_t>(hint_sep_y), pw, 2, kFooter);

    if (!chrome.footer_hint_visible) return;

    static constexpr const char* kLeft = "esc interrupt  pgup/dn scroll";
    static constexpr const char* kRight = "/agents list agents  /help list commands";
    const int kLeftVis = 29;
    const int kRightVis = 40;
    const bool show_left = r.w >= kLeftVis;
    const bool show_right = r.w >= kLeftVis + 1 + kRightVis;
    const int hint_pad = r.w - (show_left ? kLeftVis : 0) - (show_right ? kRightVis : 0);
    std::uint32_t hx = px;
    if (show_left) {
        draw_text(frame, hx, static_cast<std::uint32_t>(hint_y), kLeft, dim, kFooter);
        hx += static_cast<std::uint32_t>(kLeftVis);
    }
    if (hint_pad > 0) {
        std::string spaces(static_cast<size_t>(hint_pad), ' ');
        draw_text(frame, hx, static_cast<std::uint32_t>(hint_y), spaces, dim, kFooter);
        hx += static_cast<std::uint32_t>(hint_pad);
    }
    if (show_right) {
        draw_text(frame, hx, static_cast<std::uint32_t>(hint_y), kRight, dim, kFooter);
    }
}

} // namespace arbiter::opentui
