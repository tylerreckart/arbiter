// index_ai/src/tui/tui.cpp — see tui/tui.h

#include "tui/tui.h"
#include "cli_helpers.h"   // term_cols / term_rows
#include "theme.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace index_ai {

// ─── TUI ─────────────────────────────────────────────────────────────────────

void TUI::enter_alt_screen() {
    ::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("\033[?1049h");   // enter alternate screen
    std::printf("\033[2J");       // clear
    // NOTE: X10 mouse reporting is intentionally NOT enabled.  Capturing
    // mouse events disables native text selection in the terminal, and being
    // able to copy output matters more than wheel-scrolling our scroll region.
    // PgUp / PgDn drive the scroll handler from the keyboard instead.
    std::fflush(stdout);
}

void TUI::leave_alt_screen() {
    // Belt-and-suspenders: if a prior version (or a misbehaving terminal)
    // left mouse reporting on, turn it back off before leaving alt-screen.
    // Also force cursor visible — we hide/show it around paints so the
    // focused input is the only place it lands, but on shutdown we hand
    // the terminal back to the user and they expect a visible cursor.
    std::printf("\033[?1000l");
    std::printf("\033[?25h");
    std::printf("\033[?1049l");
    std::fflush(stdout);
}

void TUI::init(const std::string& agent,
               const std::string& /*model*/,
               const std::string& color) {
    // Default to a full-screen rect.  Multi-pane callers follow this up with
    // set_rect(bounds) after layout computes their share.
    rect_ = Rect{0, 0, term_cols(), term_rows()};
    current_agent_ = agent;
    paint_chrome();
    std::printf("\033[%d;%dH", scroll_top_row(), left_col());
    std::fflush(stdout);
}

void TUI::set_rect(const Rect& r) {
    rect_ = r;
    paint_chrome();
}

void TUI::resize() {
    // Single-pane callers use this after SIGWINCH; multi-pane callers go
    // through the layout tree (which calls clear-once then set_rect on each
    // pane).  Full-clear before repaint so a shrink doesn't leave orphaned
    // chrome rows past the new bottom.
    rect_.w = term_cols();
    rect_.h = term_rows();
    std::printf("\033[2J");
    paint_chrome();
}

void TUI::shutdown() {
    // No-op today.  enter_alt_screen / leave_alt_screen are now static and
    // owned by the app's entry path; this stays for source-compat with
    // callers that already pair it with init.
}

void TUI::paint_chrome() {
    draw_header();
    draw_sep();
    erase_chrome_row(input_row());
    draw_footer_hint();
}

void TUI::update(const std::string& agent,
                 const std::string& /*model*/,
                 const std::string& stats,
                 const std::string& color) {
    current_agent_ = agent;
    current_stats_ = stats;
    draw_header();
}

void TUI::draw_sep() {
    // Locking is required because ToolCallIndicator's spinner thread calls
    // us via set_pre_input_status while the main thread may also be redrawing
    // chrome.  tty_mu_ is recursive, so nested calls from begin_input /
    // grow_input (which already hold the lock) are fine.
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    const Theme& t = theme();
    std::printf("\0337\033[?25l");
    erase_pane_row(sep_row());
    // Top border of the readline.  In multi-pane mode the focused pane
    // accents its border; everyone else stays dim.  Single-pane always
    // dim — there's nothing to contrast with.
    const std::string& top_border =
        focus_accent_ ? t.border_active : t.border_inactive;
    std::printf("%s", top_border.c_str());
    if (current_pre_input_status_.empty()) {
        for (int i = 0; i < rect_.w; ++i) std::printf("─");
    } else {
        auto cell_w = [](const std::string& s) {
            int w = 0;
            for (unsigned char c : s)
                if ((c & 0xC0) != 0x80) ++w;
            return w;
        };
        // Two-space gap on each side so the label reads as inset, not fused
        // with the dashes.  The remaining width fills with ─ so the row still
        // reads as a separator.
        std::printf("  %s  ", current_pre_input_status_.c_str());
        int used = 4 + cell_w(current_pre_input_status_);
        for (int i = used; i < rect_.w; ++i) std::printf("─");
    }
    std::printf("%s\0338\033[?25h", t.reset.c_str());
    std::fflush(stdout);
}

void TUI::begin_input(std::function<int()> pending_fn) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    int queued = pending_fn ? pending_fn() : 0;
    // Clear all rows the previous (possibly taller) input area might have
    // occupied, down to and including the last terminal row.
    int old_top = input_top_row();
    for (int r = old_top; r <= input_row(); ++r) erase_pane_row(r);

    input_rows_ = 1;
    draw_sep();
    erase_pane_row(input_top_row());
    std::fflush(stdout);

    if (queued > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d queued", queued);
        set_status(buf);
        queue_indicator_shown_ = true;
    } else if (queue_indicator_shown_) {
        clear_status();
    }
}

void TUI::grow_input(int needed) {
    needed = std::max(1, std::min(needed, kMaxInputRows));
    if (needed == input_rows_) return;

    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    // Clear every row that was part of the input area under the old layout,
    // AND every row that will be under the new layout.  That covers both the
    // grow case (new rows reclaimed from scroll region, need to be blank)
    // and the shrink case (rows we're releasing back to the scroll region
    // shouldn't carry stale input text into the scroll view).
    int old_top = input_top_row();
    int new_top = rect_.y + rect_.h - kBottomPadRows - needed + 1;
    int clear_top = std::min(old_top, new_top);
    for (int r = clear_top; r <= input_row(); ++r) erase_pane_row(r);

    input_rows_ = needed;
    draw_sep();
    std::printf("\033[%d;%dH", input_top_row(), left_col());
    std::fflush(stdout);
}

std::string TUI::build_prompt() const {
    // \001…\002 wraps mark LineEditor invisible-width bytes so the prompt
    // color sequence doesn't count toward the visible width used for
    // wrapping/cursor arithmetic.
    const Theme& t = theme();
    return "\001" + t.prompt_color + "\002>\001" + t.reset + "\002 ";
}

void TUI::render_scrollback(const ScrollBuffer& buf,
                            int visual_offset, int new_count) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    buf.render(left_col(), scroll_top_row(), rect_.w,
               last_scroll_row(), visual_offset);

    if (visual_offset > 0) {
        char sbuf[96];
        if (new_count > 0)
            std::snprintf(sbuf, sizeof(sbuf),
                          "↑ %d rows above  ·  %d new  [PgDn]", visual_offset, new_count);
        else
            std::snprintf(sbuf, sizeof(sbuf),
                          "↑ %d rows above  [PgDn to return]", visual_offset);
        set_status(sbuf);
    }
}

void TUI::set_status(const std::string& msg) {
    current_status_ = msg;
    status_active_ = true;
    draw_header();
}

void TUI::clear_status() {
    if (!status_active_) return;
    current_status_.clear();
    status_active_ = false;
    queue_indicator_shown_ = false;
    draw_header();
}

void TUI::clear_queue_indicator() {
    if (queue_indicator_shown_) clear_status();
}

void TUI::set_pre_input_status(const std::string& msg) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    if (current_pre_input_status_ == msg) return;
    current_pre_input_status_ = msg;
    draw_sep();
}

void TUI::clear_pre_input_status() {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    if (current_pre_input_status_.empty()) return;
    current_pre_input_status_.clear();
    draw_sep();
}

void TUI::set_title(const std::string& title) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    std::lock_guard<std::mutex> lk(header_mu_);
    session_title_ = title;
    draw_header_locked();
}

void TUI::erase_chrome_row(int row) {
    std::printf("\0337\033[?25l");
    erase_pane_row(row);
    std::printf("\0338\033[?25h");
    std::fflush(stdout);
}

void TUI::erase_pane_row(int row) {
    // Pane-aware replacement for \033[2K.  Writes exactly rect_.w spaces
    // starting at the pane's left column, then leaves the cursor sitting at
    // that same column of `row` so the next write lands correctly.  \033[0m
    // resets attributes first so the spaces don't inherit background color
    // from whatever preceded this call.
    std::printf("\033[%d;%dH\033[0m", row, left_col());
    for (int i = 0; i < rect_.w; ++i) std::putc(' ', stdout);
    std::printf("\033[%d;%dH", row, left_col());
}

void TUI::draw_header() {
    // tty_mu_ first (it's the outer lock — held whenever stdout is touched),
    // then header_mu_ (inner — guards only the header text cache).  Always
    // acquire in this order to avoid a classic AB/BA deadlock.
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    std::lock_guard<std::mutex> lk(header_mu_);
    draw_header_locked();
}

void TUI::draw_header_locked() {
    // UTF-8 cell-width (not byte count) — the thinking-indicator spinner uses
    // 3-byte braille glyphs that render as 1 cell.  Computing pad from bytes
    // under-pads, leaving visible whitespace to the right of the status.
    auto cell_w = [](const std::string& s) {
        int w = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80) ++w;  // non-continuation byte
        return w;
    };

    // Truncate the agent name if it alone would overflow the pane.
    std::string agent_vis = current_agent_;
    if (cell_w(agent_vis) > rect_.w) {
        agent_vis.resize(std::max(0, rect_.w));
    }

    int left_w = (int)agent_vis.size() + 2;

    // Title only fits if there's room for it; drop it entirely in narrow panes.
    std::string title_vis;
    if (!session_title_.empty() && left_w + 1 + (int)session_title_.size() <= rect_.w) {
        title_vis = session_title_;
        left_w += 1 + (int)title_vis.size();
    }

    const bool have_status = !current_status_.empty();
    const std::string& right_text = have_status ? current_status_ : current_stats_;
    std::string right_vis = right_text;
    int avail = std::max(0, rect_.w - left_w);
    int right_cells = cell_w(right_vis);
    if (right_cells > avail) {
        right_vis.resize(avail);
        right_cells = cell_w(right_vis);
    }
    int pad = avail - right_cells;

    std::printf("\0337\033[?25l");
    // Row 1 — identity on the left, status-or-stats on the right.
    erase_pane_row(identity_row());
    std::printf("%s", agent_vis.c_str());
    if (!title_vis.empty())
        std::printf(" \033[2m%s\033[0m", title_vis.c_str());
    std::printf("  ");
    std::printf("%*s", pad, "");
    if (!right_vis.empty())
        std::printf("\033[2m%s\033[0m", right_vis.c_str());

    // Row 2 — separator.  In a multi-pane layout the focused pane
    // highlights its header bottom border in the theme accent so the
    // user can spot the active pane at a glance.  In single-pane mode
    // focus_accent_ stays false and the separator renders in the
    // border-inactive tone.
    erase_pane_row(header_sep_row());
    {
        const Theme& t = theme();
        std::printf("%s",
                    focus_accent_ ? t.border_active.c_str()
                                  : t.border_inactive.c_str());
        for (int i = 0; i < rect_.w; ++i) std::printf("─");
        std::printf("%s", t.reset.c_str());
    }

    std::printf("\0338\033[?25h");
    std::fflush(stdout);
}

void TUI::draw_footer_hint() {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    static constexpr const char* kLeft  = "esc \033[2minterrupt\033[0m  "
                                          "pgup/dn \033[2mscroll\033[0m";
    static constexpr int         kLeftVis  = 4 + 9 + 2 + 8 + 6;  // 29
    static constexpr const char* kRight = "/agents \033[2mlist agents\033[0m  "
                                          "/help \033[2mlist commands\033[0m";
    static constexpr int         kRightVis = 8 + 11 + 2 + 6 + 13; // 40

    std::printf("\0337\033[?25l");

    if (!footer_hint_visible_) {
        // Multi-pane layout.  No keybind hint, but the focused pane draws
        // a dashed bottom border in the accent color so the active
        // readline reads as a framed box (top sep_row accent + bottom
        // hint_sep_row accent).  Unfocused panes leave hint_sep_row
        // blank to keep them visually quiet.  pad_row is always blank.
        // The rows are still reserved (input_row() and sep_row() refs
        // assume a two-row bottom pad), so the readline's absolute
        // position stays identical to single-pane mode.
        erase_pane_row(hint_sep_row());
        if (focus_accent_) {
            const Theme& t = theme();
            std::printf("%s", t.border_active.c_str());
            for (int i = 0; i < rect_.w; ++i) std::printf("─");
            std::printf("%s", t.reset.c_str());
        }
        erase_pane_row(pad_row());
        std::printf("\0338\033[?25h");
        std::fflush(stdout);
        return;
    }

    // Hint separator row — full-width dashes.
    erase_pane_row(hint_sep_row());
    {
        const Theme& t = theme();
        std::printf("%s", t.border_inactive.c_str());
        for (int i = 0; i < rect_.w; ++i) std::printf("─");
        std::printf("%s", t.reset.c_str());
    }

    // Hint text row — drop kRight (then kLeft) as the pane shrinks so the
    // composed line strictly fits in rect_.w.  Without this guard, narrow
    // panes wrap the hint text past their right edge into the sibling.
    erase_pane_row(pad_row());
    bool show_left  = rect_.w >= kLeftVis;
    bool show_right = rect_.w >= kLeftVis + 1 + kRightVis;
    int pad = rect_.w
            - (show_left  ? kLeftVis  : 0)
            - (show_right ? kRightVis : 0);
    if (show_left)  std::fputs(kLeft, stdout);
    if (pad > 0)    std::printf("%*s", pad, "");
    if (show_right) std::fputs(kRight, stdout);
    std::printf("\033[0m\0338\033[?25h");
    std::fflush(stdout);
}

void TUI::set_footer_hint_visible(bool visible) {
    if (footer_hint_visible_ == visible) return;
    footer_hint_visible_ = visible;
    draw_footer_hint();
}

void TUI::set_focus_accent(bool active) {
    if (focus_accent_ == active) return;
    focus_accent_ = active;
    // Repaint all three borders that change color with focus:
    //   1. Header bottom sep  (draw_header → draw_header_locked)
    //   2. Readline top    sep  (draw_sep on sep_row)
    //   3. Readline bottom sep  (draw_footer_hint on hint_sep_row, multi-pane only)
    draw_header();
    draw_sep();
    draw_footer_hint();
}

void TUI::clear_input_area() {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    std::printf("\0337\033[?25l");
    // sep_row() is the dashed mid-separator just above the input;
    // input_row() is the bottom row of the input area (row N-2 in the
    // single-pane layout).  Clearing everything from sep_row through
    // input_row leaves the pane's chrome intact — the sep is rewritten
    // blank, and the next begin_input on re-focus will redraw the
    // separator + prompt from scratch.
    for (int r = sep_row(); r <= input_row(); ++r) erase_pane_row(r);
    std::printf("\0338\033[?25h");
    std::fflush(stdout);
}

void TUI::paint_idle_input_prompt() {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    std::printf("\0337\033[?25l");
    // Clear any previous input contents first, then write a dim "> " so
    // the pane visibly owns an input row even though its LineEditor is
    // dormant.  On re-focus begin_input overwrites the stub with the
    // live prompt; until then the stub stays put.
    erase_pane_row(input_top_row());
    {
        const Theme& t = theme();
        std::printf("\033[%d;%dH%s> %s",
                    input_top_row(), left_col(),
                    t.border_inactive.c_str(), t.reset.c_str());
    }
    std::printf("\0338\033[?25h");
    std::fflush(stdout);
}

// ─── Scroll-region utilities ─────────────────────────────────────────────────

void TUI::clear_scroll_region() {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    std::printf("\0337\033[?25l");                                 // save cursor
    for (int r = scroll_top_row(); r <= last_scroll_row(); ++r) {
        erase_pane_row(r);
    }
    std::printf("\0338\033[?25h");                                 // restore cursor
    std::fflush(stdout);
}

// ─── Welcome card ────────────────────────────────────────────────────────────

void TUI::draw_welcome(ScrollBuffer& history) {
    static const char* kArt[3] = {
        " \u2593\u2588\u2588\u2588\u2588\u2593 ", //  ▓████▓
        "\u2591\u2592 \u2588\u2588 \u2592\u2591", // ░▒ ██ ▒░
        " \u2593\u2588\u2580\u2580\u2588\u2593 ", //  ▓█▀▀█▓
    };
    static constexpr int kArtCells = 8;

    static const char* kText[3] = {
        "hello, i am index-arbiter's system orchestrator.",
        "",
        "what would you like to accomplish today?",
    };

    auto cell_w = [](const char* s) {
        int w = 0;
        for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
            if ((*p & 0xC0) != 0x80) ++w;   // skip continuation bytes
        }
        return w;
    };

    int text_w = 0;
    for (auto* t : kText) { int w = cell_w(t); if (w > text_w) text_w = w; }

    // Two-column interior: [pad sigil pad] │ [pad text pad]
    static constexpr int kPadL = 2, kDivGapL = 2;   // sigil column padding
    static constexpr int kDivGapR = 2, kPadR = 2;   // text  column padding
    int art_col_w  = kPadL + kArtCells + kDivGapL;
    int text_col_w = kDivGapR + text_w + kPadR;
    int inner      = art_col_w + 1 /*divider*/ + text_col_w;
    int box_w      = inner + 2;
    int margin     = std::max(0, (rect_.w - box_w) / 2);
    // Center the card horizontally within the pane's rect, not the raw
    // terminal — for panes offset from the left edge, rect_.x shifts the
    // whole card right to stay within bounds.
    std::string left_pad(rect_.x + margin, ' ');

    const char* DIM = "\033[2m";
    const char* RST = "\033[0m";

    auto border = [&](const char* l_corner, const char* junction, const char* r_corner) {
        std::string s = left_pad;
        s += DIM;
        s += l_corner;
        for (int i = 0; i < art_col_w;  ++i) s += "\u2500";    // ─
        s += junction;
        for (int i = 0; i < text_col_w; ++i) s += "\u2500";
        s += r_corner;
        s += RST;
        s += "\n";
        return s;
    };
    auto blank_row = [&]() {
        std::string s = left_pad;
        s += DIM;
        s += "\u2502";                                     // │
        s += std::string(art_col_w,  ' ');
        s += "\u2502";                                     // divider │
        s += std::string(text_col_w, ' ');
        s += "\u2502";
        s += RST;
        s += "\n";
        return s;
    };

    // Bottom border with the project version inset in the lower-right —
    // the border's ─ run is replaced by " v<version> " near the right corner
    // so it reads like a tag stamped onto the frame.  Falls back to the
    // uniform border if the version string can't fit without collision.
    auto bottom_with_version = [&](const char* version) {
        const int right_margin = 2;   // dashes between version and ╯
        std::string tag = " v";
        tag += version;
        tag += " ";
        int tag_w = cell_w(tag.c_str());
        int fill_left = text_col_w - tag_w - right_margin;
        if (fill_left < 1) {
            // No room for the inset; fall back to a clean corner.
            return border("\u2570", "\u2534", "\u256F");
        }
        std::string s = left_pad;
        s += DIM;
        s += "\u2570";                                     // ╰
        for (int i = 0; i < art_col_w; ++i) s += "\u2500";
        s += "\u2534";                                     // ┴
        for (int i = 0; i < fill_left;  ++i) s += "\u2500";
        s += tag;
        for (int i = 0; i < right_margin; ++i) s += "\u2500";
        s += "\u256F";                                     // ╯
        s += RST;
        s += "\n";
        return s;
    };

    std::string card;
    card += border("\u256D", "\u252C", "\u256E");          // ╭ ┬ ╮
    card += blank_row();
    for (int i = 0; i < 3; ++i) {
        std::string s = left_pad;
        s += DIM; s += "\u2502"; s += RST;                 // left border
        s += std::string(kPadL, ' ');
        s += kArt[i];
        s += std::string(kDivGapL, ' ');
        s += DIM; s += "\u2502"; s += RST;                 // divider
        s += std::string(kDivGapR, ' ');
        s += kText[i];
        s += std::string(text_w - cell_w(kText[i]) + kPadR, ' ');
        s += DIM; s += "\u2502"; s += RST;                 // right border
        s += "\n";
        card += s;
    }
    card += blank_row();
#ifdef INDEX_VERSION
    card += bottom_with_version(INDEX_VERSION);
#else
    card += border("\u2570", "\u2534", "\u256F");
#endif

    // Vertically center the card in the scroll region.  The centering is
    // purely visual — we position the cursor at the centered row rather than
    // prepending newlines to the content, so the scroll buffer stays clean
    // and the card is cleared entirely on first user input.
    int card_h = 7;  // top border + blank + 3 content + blank + bottom border
    int start_row = scroll_top_row() +
                    std::max(0, (scroll_region_rows() - card_h) / 2);

    history.push(card);

    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    std::printf("\0337\033[?25l");
    std::printf("\033[%d;1H", start_row);
    std::fwrite(card.data(), 1, card.size(), stdout);
    std::printf("\0338\033[?25h");
    std::fflush(stdout);
}

// ─── ThinkingIndicator ───────────────────────────────────────────────────────

void ThinkingIndicator::start(const std::string& label) {
    // Idempotent: cleanly stop any prior animation so callers can use
    // start(new_label) as a "switch label" operation without leaking threads.
    stop();
    label_   = label;
    running_ = true;
    thread_  = std::thread([this]() {
        static const char* frames[] = {
            "\u2801", "\u2802", "\u2804", "\u2840", "\u2848", "\u2850",
            "\u2860", "\u28C0", "\u28C1", "\u28C2", "\u28C4", "\u28CC",
            "\u28D4", "\u28E4", "\u28E5", "\u28E6", "\u28EE", "\u28F6",
            "\u28F7", "\u28FF", "\u287F", "\u283F", "\u281F", "\u281F",
            "\u285B", "\u281B", "\u282B", "\u288B", "\u280B", "\u280D",
            "\u2809", "\u2809", "\u2811", "\u2821", "\u2881"
        };
        static const int kFrames = sizeof(frames) / sizeof(frames[0]);
        int i = 0;
        while (running_.load()) {
            if (tui_) tui_->set_status(label_ + " " + frames[i % kFrames]);
            ++i;
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }
        if (tui_) tui_->clear_status();
    });
}

void ThinkingIndicator::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// ─── ToolCallIndicator ───────────────────────────────────────────────────────

namespace {
// Shared braille frames — matches ThinkingIndicator so the two indicators
// feel visually related when they switch places during a turn.
static const char* kToolFrames[] = {
    "\u2801", "\u2802", "\u2804", "\u2840", "\u2848", "\u2850",
    "\u2860", "\u28C0", "\u28C1", "\u28C2", "\u28C4", "\u28CC",
    "\u28D4", "\u28E4", "\u28E5", "\u28E6", "\u28EE", "\u28F6",
    "\u28F7", "\u28FF", "\u287F", "\u283F", "\u281F", "\u281F",
    "\u285B", "\u281B", "\u282B", "\u288B", "\u280B", "\u280D",
    "\u2809", "\u2809", "\u2811", "\u2821", "\u2881"
};
static constexpr int kToolFramesCount =
    sizeof(kToolFrames) / sizeof(kToolFrames[0]);
}

void ToolCallIndicator::begin() {
    // Idempotent: if a previous turn was never finalized (shouldn't happen,
    // but be defensive), tear it down before re-arming.
    if (running_.load() || thread_.joinable()) {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    armed_.store(true);
    total_.store(0);
    failed_.store(0);
}

void ToolCallIndicator::bump(const std::string& /*kind*/, bool ok) {
    if (!armed_.load()) return;
    total_.fetch_add(1);
    if (!ok) failed_.fetch_add(1);
    // First bump starts the spinner — delays spinner start until there's
    // actually something to count, so a zero-tool-call turn never paints.
    if (!running_.load()) start_spinner();
    // Immediate repaint so the count advances the moment bump fires; the
    // spinner thread then keeps the glyph animating at 80 ms cadence.
    render_status();
}

void ToolCallIndicator::start_spinner() {
    running_ = true;
    thread_ = std::thread([this]() {
        while (running_.load()) {
            render_status();
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }
    });
}

void ToolCallIndicator::render_status() {
    if (!tui_) return;
    // Advance the frame on every render (both timer-driven and bump-driven);
    // a static counter here is safe because render_status only runs from the
    // spinner thread after start_spinner, and from bump() which is exec-thread
    // only — they don't race, and a one-frame jitter on the boundary is fine.
    static thread_local int frame = 0;
    int n = total_.load();
    int f = failed_.load();
    std::string label = kToolFrames[(frame++) % kToolFramesCount];
    label += " ";
    label += std::to_string(n);
    label += " tool call";
    if (n != 1) label += "s";
    label += "\u2026"; // ellipsis
    if (f > 0) {
        label += " (";
        label += std::to_string(f);
        label += " failed)";
    }
    tui_->set_pre_input_status(label);
}

std::string ToolCallIndicator::finalize() {
    if (!armed_.load()) return "";
    armed_ = false;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (tui_) tui_->clear_pre_input_status();

    int n = total_.load();
    int f = failed_.load();
    if (n == 0) return "";

    // Dim summary line — ✓ green if clean, ✗ red if any call failed.  One
    // trailing newline so OutputQueue::end_message() gets the expected
    // separation before the agent's synthesis renders.
    const Theme& t = theme();
    std::string out;
    if (f == 0) {
        out += t.accent_success + "\u2713" + t.reset + " ";
    } else {
        out += t.accent_error   + "\u2717" + t.reset + " ";
    }
    out += t.dim;
    out += std::to_string(n);
    out += " tool call";
    if (n != 1) out += "s";
    if (f > 0) {
        out += " (";
        out += std::to_string(f);
        out += " failed)";
    }
    out += "\033[0m\n";
    return out;
}

} // namespace index_ai
