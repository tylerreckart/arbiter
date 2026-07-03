#include "tui/opentui/pane_frame.h"

#include "tui/opentui/engine.h"
#include "theme.h"

#include <algorithm>
#include <string>

namespace arbiter::opentui {

namespace {

using Rgba = std::array<std::uint16_t, 4>;

Rgba rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return rgba8(r, g, b);
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
               const Rgba& fg,
               const Rgba* bg = nullptr) {
    if (text.empty()) return;
    bufferDrawText(frame,
                   text.data(),
                   static_cast<std::uint32_t>(text.size()),
                   x,
                   y,
                   fg.data(),
                   bg ? bg->data() : nullptr,
                   0);
}

void fill_row(OpenTuiHandle frame,
              std::uint32_t x,
              std::uint32_t y,
              std::uint32_t w,
              const Rgba& bg) {
    if (w == 0) return;
    bufferFillRect(frame, x, y, w, 1, bg.data());
}

void draw_dashes(OpenTuiHandle frame,
                 std::uint32_t x,
                 std::uint32_t y,
                 std::uint32_t w,
                 const Rgba& fg,
                 const Rgba& bg) {
    fill_row(frame, x, y, w, bg);
    if (w == 0) return;
    std::string dashes;
    dashes.reserve(static_cast<size_t>(w) * 3);
    for (std::uint32_t i = 0; i < w; ++i) dashes += "\u2500";
    draw_text(frame, x, y, dashes, fg, &bg);
}

} // namespace

void draw_pane_chrome(OpenTuiHandle frame, const TUI& tui) {
    const TuiChromeSnapshot chrome = tui.chrome_snapshot();
    const Rect& r = chrome.rect;
    if (r.w <= 0 || r.h <= 0) return;

    const Rgba bg = rgb(0x1e, 0x1e, 0x2e);
    const Rgba scroll_bg = rgb(0x18, 0x18, 0x25);
    const Rgba fg = rgb(0xab, 0xb2, 0xbf);
    const Rgba dim = rgb(0x5c, 0x63, 0x70);
    const Rgba border = chrome.focus_accent ? rgb(0x61, 0xaf, 0xef) : rgb(0x3e, 0x44, 0x52);

    const std::uint32_t px = static_cast<std::uint32_t>(r.x);
    const std::uint32_t pw = static_cast<std::uint32_t>(r.w);
    const std::uint32_t ph = static_cast<std::uint32_t>(r.h);

    bufferFillRect(frame, px, static_cast<std::uint32_t>(r.y), pw, ph, bg.data());

    const int identity_y = r.y;
    const int header_sep_y = r.y + 1;
    const int sep_y = r.y + r.h - TUI::kBottomPadRows - chrome.input_rows - TUI::kSepRows;
    const int input_top_y = sep_y + 1;
    const int input_bottom_y = r.y + r.h - TUI::kBottomPadRows - 1;
    const int hint_sep_y = r.y + r.h - 2;
    const int hint_y = r.y + r.h - 1;
    const int scroll_top_y = r.y + TUI::kHeaderRows;
    const int scroll_bottom_y = sep_y - 1;

    if (scroll_top_y <= scroll_bottom_y) {
        const std::uint32_t scroll_h =
            static_cast<std::uint32_t>(scroll_bottom_y - scroll_top_y + 1);
        bufferFillRect(frame,
                       px,
                       static_cast<std::uint32_t>(scroll_top_y),
                       pw,
                       scroll_h,
                       scroll_bg.data());
    }

    // Header identity row.
    fill_row(frame, px, static_cast<std::uint32_t>(identity_y), pw, bg);
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

    draw_text(frame, px, static_cast<std::uint32_t>(identity_y), agent, fg, &bg);
    std::uint32_t cx = px + static_cast<std::uint32_t>(cell_width(agent));
    if (!title.empty()) {
        draw_text(frame, cx, static_cast<std::uint32_t>(identity_y), " ", fg, &bg);
        cx += 1;
        draw_text(frame, cx, static_cast<std::uint32_t>(identity_y), title, dim, &bg);
        cx += static_cast<std::uint32_t>(cell_width(title));
    }
    cx += static_cast<std::uint32_t>(2 + pad);
    if (!right.empty()) {
        draw_text(frame, cx, static_cast<std::uint32_t>(identity_y), right, dim, &bg);
    }

    draw_dashes(frame, px, static_cast<std::uint32_t>(header_sep_y), pw, border, bg);

    // Mid separator + optional tool-call label.
    fill_row(frame, px, static_cast<std::uint32_t>(sep_y), pw, bg);
    if (chrome.pre_input_status.empty()) {
        draw_dashes(frame, px, static_cast<std::uint32_t>(sep_y), pw, border, bg);
    } else {
        const std::string gap = "  ";
        const int label_cells = cell_width(chrome.pre_input_status);
        const int used = 4 + label_cells;
        draw_text(frame, px, static_cast<std::uint32_t>(sep_y), gap, border, &bg);
        draw_text(frame,
                  px + 2,
                  static_cast<std::uint32_t>(sep_y),
                  chrome.pre_input_status,
                  dim,
                  &bg);
        const int dash_cells = std::max(0, r.w - used);
        if (dash_cells > 0) {
            std::string tail;
            tail.reserve(static_cast<size_t>(dash_cells) * 3);
            for (int i = 0; i < dash_cells; ++i) tail += "\u2500";
            draw_text(frame,
                      px + static_cast<std::uint32_t>(used - 2),
                      static_cast<std::uint32_t>(sep_y),
                      tail,
                      border,
                      &bg);
        }
    }

    // Input background (PaneInputEditor draws prompt + buffer on top).
    if (input_top_y <= input_bottom_y) {
        bufferFillRect(frame,
                       px,
                       static_cast<std::uint32_t>(input_top_y),
                       pw,
                       static_cast<std::uint32_t>(input_bottom_y - input_top_y + 1),
                       bg.data());
    }

    // Footer hint rows.
    if (!chrome.footer_hint_visible) {
        if (chrome.focus_accent) {
            draw_dashes(frame, px, static_cast<std::uint32_t>(hint_sep_y), pw, border, bg);
        } else {
            fill_row(frame, px, static_cast<std::uint32_t>(hint_sep_y), pw, bg);
        }
        fill_row(frame, px, static_cast<std::uint32_t>(hint_y), pw, bg);
        return;
    }

    draw_dashes(frame, px, static_cast<std::uint32_t>(hint_sep_y), pw, border, bg);

    static constexpr const char* kLeft = "esc interrupt  pgup/dn scroll";
    static constexpr const char* kRight = "/agents list agents  /help list commands";
    const int kLeftVis = 29;
    const int kRightVis = 40;
    fill_row(frame, px, static_cast<std::uint32_t>(hint_y), pw, bg);
    const bool show_left = r.w >= kLeftVis;
    const bool show_right = r.w >= kLeftVis + 1 + kRightVis;
    const int hint_pad = r.w - (show_left ? kLeftVis : 0) - (show_right ? kRightVis : 0);
    std::uint32_t hx = px;
    if (show_left) {
        draw_text(frame, hx, static_cast<std::uint32_t>(hint_y), kLeft, dim, &bg);
        hx += static_cast<std::uint32_t>(kLeftVis);
    }
    if (hint_pad > 0) {
        std::string spaces(static_cast<size_t>(hint_pad), ' ');
        draw_text(frame, hx, static_cast<std::uint32_t>(hint_y), spaces, dim, &bg);
        hx += static_cast<std::uint32_t>(hint_pad);
    }
    if (show_right) {
        draw_text(frame, hx, static_cast<std::uint32_t>(hint_y), kRight, dim, &bg);
    }
}

} // namespace arbiter::opentui
