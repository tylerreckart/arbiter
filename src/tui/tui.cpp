// index_ai/src/tui/tui.cpp — see tui/tui.h

#include "tui/tui.h"
#include "cli_helpers.h"   // term_cols / term_rows

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace index_ai {

// ─── TUI ─────────────────────────────────────────────────────────────────────

void TUI::init(const std::string& agent,
               const std::string& /*model*/,
               const std::string& color) {
    ::setvbuf(stdout, nullptr, _IONBF, 0);

    cols_ = term_cols();
    rows_ = term_rows();
    current_agent_ = agent;

    std::printf("\033[?1049h");   // enter alternate screen
    std::printf("\033[2J");       // clear
    // NOTE: X10 mouse reporting is intentionally NOT enabled.  Capturing
    // mouse events disables native text selection in the terminal, and being
    // able to copy output matters more than wheel-scrolling our scroll region.
    // PgUp / PgDn drive the scroll handler from the keyboard instead.
    set_scroll_region();
    std::fflush(stdout);
    draw_header();
    draw_sep();
    erase_chrome_row(input_row());
    draw_footer_hint();
    std::printf("\033[%d;1H", kHeaderRows + 1);
    std::fflush(stdout);
}

void TUI::resize() {
    cols_ = term_cols();
    rows_ = term_rows();
    std::printf("\033[2J");
    set_scroll_region();
    draw_header();
    draw_sep();
    erase_chrome_row(input_row());
    draw_footer_hint();
    std::fflush(stdout);
}

void TUI::shutdown() {
    // Belt-and-suspenders: if a prior version (or a misbehaving terminal)
    // left mouse reporting on, turn it back off before leaving alt-screen.
    std::printf("\033[?1000l");
    std::printf("\033[?1049l");
    std::fflush(stdout);
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
    std::printf("\0337");
    std::printf("\033[%d;1H\033[38;5;237m", sep_row());
    for (int i = 0; i < cols_; ++i) std::printf("─");
    std::printf("\033[0m");
    std::printf("\0338");
    std::fflush(stdout);
}

void TUI::begin_input(std::function<int()> pending_fn) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    int queued = pending_fn ? pending_fn() : 0;
    // Clear all rows the previous (possibly taller) input area might have
    // occupied, down to and including the last terminal row.
    int old_top = input_top_row();
    for (int r = old_top; r <= input_row(); ++r)
        std::printf("\033[%d;1H\033[2K", r);

    input_rows_ = 1;
    set_scroll_region();
    draw_sep();
    std::printf("\033[%d;1H\033[2K", input_top_row());
    std::printf("\033[%d;1H",        input_top_row());
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
    int new_top = rows_ - kBottomPadRows - needed + 1;
    int clear_top = std::min(old_top, new_top);
    for (int r = clear_top; r <= input_row(); ++r)
        std::printf("\033[%d;1H\033[2K", r);

    input_rows_ = needed;
    set_scroll_region();
    draw_sep();
    std::printf("\033[%d;1H", input_top_row());
    std::fflush(stdout);
}

std::string TUI::build_prompt() const {
    return "\001\033[38;5;241m\002>\001\033[0m\002 ";
}

void TUI::render_scrollback(const ScrollBuffer& buf,
                            int visual_offset, int new_count) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    buf.render(kHeaderRows + 1, last_scroll_row(), visual_offset);

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

void TUI::set_title(const std::string& title) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    std::lock_guard<std::mutex> lk(header_mu_);
    session_title_ = title;
    draw_header_locked();
}

void TUI::set_scroll_region() {
    int top = kHeaderRows + 1;
    int bot = last_scroll_row();
    std::printf("\033[%d;%dr", top, bot);
}

void TUI::erase_chrome_row(int row) {
    std::printf("\0337");
    std::printf("\033[%d;1H\033[2K\033[0m", row);
    std::printf("\0338");
    std::fflush(stdout);
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

    int left_w = (int)current_agent_.size() + 2;
    if (!session_title_.empty())
        left_w += 1 + (int)session_title_.size();

    const bool have_status = !current_status_.empty();
    const std::string& right_text = have_status ? current_status_ : current_stats_;
    std::string right_vis = right_text;
    int avail = std::max(0, cols_ - left_w);
    int right_cells = cell_w(right_vis);
    if (right_cells > avail) {
        // Truncate to `avail` cells (byte-safe for ASCII; multi-byte status
        // tails never exceed avail in practice, so a byte-resize is fine).
        right_vis.resize(avail);
        right_cells = cell_w(right_vis);
    }
    int pad = avail - right_cells;

    std::printf("\0337");
    // Row 1 — identity on the left, status-or-stats on the right.
    std::printf("\033[%d;1H\033[2K", kIdentityRow);
    std::printf("%s", current_agent_.c_str());
    if (!session_title_.empty())
        std::printf(" \033[2m%s\033[0m", session_title_.c_str());
    std::printf("  ");
    std::printf("%*s", pad, "");
    if (!right_vis.empty())
        std::printf("\033[2m%s\033[0m", right_vis.c_str());

    // Row 2 — separator
    std::printf("\033[%d;1H\033[2K\033[38;5;237m", kHeaderSepRow);
    for (int i = 0; i < cols_; ++i) std::printf("─");
    std::printf("\033[0m");

    std::printf("\0338");
    std::fflush(stdout);
}

void TUI::draw_footer_hint() {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    static constexpr const char* kLeft  = "esc \033[2minterrupt\033[0m  "
                                          "pgup/dn \033[2mscroll\033[0m";
    static constexpr int         kLeftVis  = 4 + 9 + 2 + 8 + 6;
    static constexpr const char* kRight = "/agents \033[2mlist agents\033[0m  "
                                          "/help \033[2mlist commands\033[0m";
    static constexpr int         kRightVis = 8 + 11 + 2 + 6 + 13;
    int pad = std::max(1, cols_ - kLeftVis - kRightVis);
    std::printf("\0337");
    std::printf("\033[%d;1H\033[2K\033[38;5;237m", hint_sep_row());
    for (int i = 0; i < cols_; ++i) std::printf("─");
    std::printf("\033[0m");
    std::printf("\033[%d;1H\033[2K", pad_row());
    std::printf("%s%*s%s", kLeft, pad, "", kRight);
    std::printf("\033[0m\0338");
    std::fflush(stdout);
}

// ─── Scroll-region utilities ─────────────────────────────────────────────────

void TUI::clear_scroll_region() {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    std::printf("\0337");                                 // save cursor
    for (int r = kHeaderRows + 1; r <= last_scroll_row(); ++r) {
        std::printf("\033[%d;1H\033[2K", r);              // goto row, clear line
    }
    std::printf("\0338");                                 // restore cursor
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
        "hello, i am index.",
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
    int margin     = std::max(0, (cols_ - box_w) / 2);
    std::string left_pad(margin, ' ');

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
    int start_row = kHeaderRows + 1 +
                    std::max(0, (scroll_region_rows() - card_h) / 2);

    history.push(card);

    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    std::printf("\0337");
    std::printf("\033[%d;1H", start_row);
    std::fwrite(card.data(), 1, card.size(), stdout);
    std::printf("\0338");
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
    tui_->set_status(label);
}

std::string ToolCallIndicator::finalize() {
    if (!armed_.load()) return "";
    armed_ = false;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (tui_) tui_->clear_status();

    int n = total_.load();
    int f = failed_.load();
    if (n == 0) return "";

    // Dim summary line — ✓ green if clean, ✗ red if any call failed.  One
    // trailing newline so OutputQueue::end_message() gets the expected
    // separation before the agent's synthesis renders.
    std::string out;
    if (f == 0) {
        out += "\033[38;5;108m\u2713\033[0m "; // green check
    } else {
        out += "\033[38;5;167m\u2717\033[0m "; // red x
    }
    out += "\033[2m";
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
