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

void draw_welcome_card(OpenTuiHandle frame, const TUI& tui) {
    const TuiChromeSnapshot chrome = tui.chrome_snapshot();
    const Rect& r = chrome.rect;
    if (r.w <= 0 || r.h <= 0) return;

    const TuiDesign& d = tui_design();
    const int raw_pad = (r.w <= d.layout.dense_cols) ? 0 : std::max(0, d.layout.pane_padding_x);
    const int pad = std::min(raw_pad, std::max(0, (r.w - 1) / 2));

    static const char* kArt[3] = {
        " \u2593\u2588\u2588\u2588\u2588\u2593 ",
        "\u2591\u2592 \u2588\u2588 \u2592\u2591",
        " \u2593\u2588\u2580\u2580\u2588\u2593 ",
    };
    static constexpr int kArtCells = 8;
    static const char* kText[3] = {
        "hello, i am index-arbiter's system orchestrator.",
        "",
        "what would you like to accomplish today?",
    };

    int text_w = 0;
    for (const char* t : kText) {
        const int w = cell_width(t);
        if (w > text_w) text_w = w;
    }

    static constexpr int kPadL = 2;
    static constexpr int kDivGapL = 2;
    static constexpr int kDivGapR = 2;
    static constexpr int kPadR = 2;
    static constexpr int kCardRows = 7;
    static constexpr int kShadowDx = 1;
    static constexpr int kShadowDy = 1;

    const int block_w = std::max(1, r.w - (pad * 2));
    const int min_inner = kPadL + kArtCells + kDivGapL + 1 + kDivGapR + kPadR + 2;
    text_w = std::max(0, std::min(text_w, block_w - min_inner - kShadowDx));
    const int art_col_w = kPadL + kArtCells + kDivGapL;
    const int text_col_w = kDivGapR + text_w + kPadR;
    const int card_w = art_col_w + text_col_w + 3;

    const int sep_y = r.y + r.h - TUI::kBottomPadRows - chrome.input_rows - TUI::kSepRows;
    const int scroll_top_y = r.y + TUI::kHeaderRows;
    const int scroll_bottom_y = sep_y - 1;
    const int scroll_h = scroll_bottom_y - scroll_top_y + 1;
    const int footprint_w = card_w + kShadowDx;
    const int footprint_h = kCardRows + kShadowDy;
    if (scroll_h < footprint_h || card_w < 5) return;

    const int card_x = r.x + pad + std::max(0, (block_w - footprint_w) / 2);
    const int card_y = scroll_top_y + std::max(0, (scroll_h - footprint_h) / 2);

    const TuiRgba& border_fg = d.text.subtle;
    // Slightly lighter than scroll so the offset reads on the dark background.
    const TuiRgba& shadow_bg = d.bg.status;

    // Drop shadow: offset fill plus right/bottom edge strips.
    fill_rect(frame,
              static_cast<std::uint32_t>(card_x + kShadowDx),
              static_cast<std::uint32_t>(card_y + kShadowDy),
              static_cast<std::uint32_t>(card_w),
              kCardRows,
              shadow_bg);
    fill_rect(frame,
              static_cast<std::uint32_t>(card_x + card_w),
              static_cast<std::uint32_t>(card_y + kShadowDy),
              kShadowDx,
              kCardRows,
              shadow_bg);
    fill_rect(frame,
              static_cast<std::uint32_t>(card_x + kShadowDx),
              static_cast<std::uint32_t>(card_y + kCardRows),
              static_cast<std::uint32_t>(card_w),
              kShadowDy,
              shadow_bg);

    fill_rect(frame,
              static_cast<std::uint32_t>(card_x),
              static_cast<std::uint32_t>(card_y),
              static_cast<std::uint32_t>(card_w),
              kCardRows,
              d.bg.header);

    const int mid_x = card_x + 1 + art_col_w;
    const int right_x = card_x + card_w - 1;

    auto draw_hline = [&](int x, int y, int count) {
        if (count <= 0) return;
        std::string h;
        h.reserve(static_cast<size_t>(count) * 3);
        for (int i = 0; i < count; ++i) h += d.border.horizontal;
        draw_text(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(y),
                  h,
                  border_fg,
                  d.bg.header);
    };

    auto draw_blank_row = [&](int y) {
        draw_text(frame,
                  static_cast<std::uint32_t>(card_x),
                  static_cast<std::uint32_t>(y),
                  d.border.vertical,
                  border_fg,
                  d.bg.header);
        draw_text(frame,
                  static_cast<std::uint32_t>(mid_x),
                  static_cast<std::uint32_t>(y),
                  d.border.vertical,
                  border_fg,
                  d.bg.header);
        draw_text(frame,
                  static_cast<std::uint32_t>(right_x),
                  static_cast<std::uint32_t>(y),
                  d.border.vertical,
                  border_fg,
                  d.bg.header);
    };

    // Top border.
    {
        const int y = card_y;
        int x = card_x;
        draw_text(frame, static_cast<std::uint32_t>(x++), static_cast<std::uint32_t>(y),
                  "\u256D", border_fg, d.bg.header);
        draw_hline(x, y, art_col_w);
        x += art_col_w;
        draw_text(frame, static_cast<std::uint32_t>(x++), static_cast<std::uint32_t>(y),
                  "\u252C", border_fg, d.bg.header);
        draw_hline(x, y, text_col_w);
        draw_text(frame,
                  static_cast<std::uint32_t>(card_x + card_w - 1),
                  static_cast<std::uint32_t>(y),
                  "\u256E",
                  border_fg,
                  d.bg.header);
    }

    draw_blank_row(card_y + 1);

    const int art_x = card_x + 1 + kPadL;
    const int text_x = mid_x + 1 + kDivGapR;
    for (int i = 0; i < 3; ++i) {
        const int y = card_y + 2 + i;
        draw_text(frame,
                  static_cast<std::uint32_t>(card_x),
                  static_cast<std::uint32_t>(y),
                  d.border.vertical,
                  border_fg,
                  d.bg.header);
        draw_text(frame,
                  static_cast<std::uint32_t>(art_x),
                  static_cast<std::uint32_t>(y),
                  kArt[i],
                  d.accent.primary,
                  d.bg.header,
                  kAttrBold);
        draw_text(frame,
                  static_cast<std::uint32_t>(mid_x),
                  static_cast<std::uint32_t>(y),
                  d.border.vertical,
                  border_fg,
                  d.bg.header);
        if (kText[i][0] != '\0') {
            draw_text(frame,
                      static_cast<std::uint32_t>(text_x),
                      static_cast<std::uint32_t>(y),
                      trim_to_cells(kText[i], text_w),
                      d.text.primary,
                      d.bg.header);
        }
        draw_text(frame,
                  static_cast<std::uint32_t>(right_x),
                  static_cast<std::uint32_t>(y),
                  d.border.vertical,
                  border_fg,
                  d.bg.header);
    }

    draw_blank_row(card_y + 5);

    // Bottom border with version tag in the text column.
#ifdef INDEX_VERSION
    const char* version = INDEX_VERSION;
#else
    const char* version = "dev";
#endif
    {
        const int y = card_y + kCardRows - 1;
        std::string tag = " v";
        tag += version;
        tag += " ";
        static constexpr int kVersionRightMargin = 2;
        const int tag_cells = cell_width(tag);
        int fill_left = text_col_w - tag_cells - kVersionRightMargin;
        if (fill_left < 1) fill_left = 1;

        int x = card_x;
        draw_text(frame, static_cast<std::uint32_t>(x++), static_cast<std::uint32_t>(y),
                  "\u2570", border_fg, d.bg.header);
        draw_hline(x, y, art_col_w);
        x += art_col_w;
        draw_text(frame, static_cast<std::uint32_t>(x++), static_cast<std::uint32_t>(y),
                  "\u2534", border_fg, d.bg.header);
        draw_hline(x, y, fill_left);
        x += fill_left;
        draw_text(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(y),
                  tag,
                  d.text.subtle,
                  d.bg.header);
        x += tag_cells;
        draw_hline(x, y, kVersionRightMargin);
        draw_text(frame,
                  static_cast<std::uint32_t>(right_x),
                  static_cast<std::uint32_t>(y),
                  "\u256F",
                  border_fg,
                  d.bg.header);
    }
}

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

    const std::string& right_src = chrome.status_active ? chrome.status : chrome.stats;
    std::string right = right_src;
    if (!right.empty() && d.layout.status_pill) {
        right = d.component.status_prefix + right + d.component.status_suffix;
    }

    const int left_chrome_w = accent_w + header_pad;
    const int right_max = std::max(0, content_w - left_chrome_w - status_inset);
    right = trim_to_cells(right, right_max);
    const int right_cells = cell_width(right);
    const int title_max = std::max(0, content_w - left_chrome_w - right_cells - status_inset);
    std::string title = chrome.title.empty() ? "Arbiter" : chrome.title;
    title = trim_to_cells(title, title_max);

    std::uint32_t cx = content_x + static_cast<std::uint32_t>(left_chrome_w);
    draw_text(frame, cx, static_cast<std::uint32_t>(header_text_y),
              title, d.text.muted, d.bg.header);

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
