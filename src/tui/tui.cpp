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
    std::printf("\033[?1000h");   // enable X10 mouse reporting
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
    int left_w = (int)current_agent_.size() + 2;
    if (!session_title_.empty())
        left_w += 1 + (int)session_title_.size();

    const bool have_status = !current_status_.empty();
    const std::string& right_text = have_status ? current_status_ : current_stats_;
    std::string right_vis = right_text;
    int avail = std::max(0, cols_ - left_w);
    if ((int)right_vis.size() > avail) right_vis.resize(avail);
    int pad = avail - (int)right_vis.size();

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
    static constexpr const char* kLeft  = "esc \033[2minterrupt\033[0m";
    static constexpr int         kLeftVis  = 4 + 9;
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

} // namespace index_ai
