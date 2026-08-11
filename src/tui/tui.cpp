// arbiter/src/tui/tui.cpp — pane layout + chrome state (OpenTUI renders pixels)

#include "tui/tui.h"
#include "cli_helpers.h"
#include "theme.h"
#include "tui/spinner.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cstdio>

namespace arbiter {

void TUI::init(const std::string& /*agent*/,
               const std::string& /*model*/,
               const std::string& /*color*/) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    rect_ = Rect{0, 0, term_cols(), term_rows()};
}

void TUI::set_rect(const Rect& r) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    rect_ = r;
}

void TUI::resize() {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    rect_.w = term_cols();
    rect_.h = term_rows();
}

int TUI::cols() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    return rect_.w;
}

int TUI::left_col() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    return rect_.x + 1;
}

int TUI::input_top_row_pub() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    return input_top_row();
}

int TUI::input_bottom_row_pub() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    return input_row();
}

int TUI::input_rows() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    return input_rows_;
}

int TUI::bottom_pad_rows() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    // Stacked panes above the outer bottom use no trailing pad — the
    // 1-cell split separator is the whole gutter (same as vertical splits).
    if (!outer_bottom_) return kCompactBottomPadRows;
    const bool visible = footer_hint_mode_ != FooterHintMode::Hidden;
    // Outer-bottom panes keep the full pad whenever the footer is enabled
    // so side-by-side column bottoms stay aligned.
    return tui_bottom_pad_rows(visible, tui_design(), /*reserve_footer_space=*/true);
}

int TUI::last_scroll_row() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    return rect_.y + rect_.h - bottom_pad_rows() - input_rows_ - kSepRows;
}

int TUI::scroll_top_row() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    // 1-indexed top of the output box. Outer-top panes float one blank row
    // (sidebar rhythm); panes below a horizontal split start flush.
    return rect_.y + 1 + (outer_top_ ? 1 : 0);
}

int TUI::scroll_region_rows() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    const int last = rect_.y + rect_.h - bottom_pad_rows() - input_rows_ - kSepRows;
    const int top  = rect_.y + 1 + (outer_top_ ? 1 : 0);
    return last - top + 1;
}

void TUI::shutdown() {}

void TUI::begin_input(std::function<int()> pending_fn) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    input_rows_ = kDefaultInputRows;

    if (pending_fn) {
        const int queued = pending_fn();
        if (queued > 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d queued", queued);
            current_status_ = buf;
            status_active_ = true;
            queue_indicator_shown_ = true;
        } else if (queue_indicator_shown_) {
            current_status_.clear();
            status_active_ = false;
            queue_indicator_shown_ = false;
        }
    }
}

void TUI::grow_input(int needed) {
    needed = std::max(kDefaultInputRows, std::min(needed, kMaxInputRows));
    if (needed == input_rows_) return;
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    input_rows_ = needed;
}

void TUI::set_input_rows(int rows) {
    rows = std::max(0, std::min(rows, kMaxInputRows));
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    input_rows_ = rows;
}

std::string TUI::build_prompt() const {
    const Theme& t = theme();
    return "\001" + t.prompt_color + "\002" + tui_design().component.prompt
         + "\001" + t.reset + "\002";
}

void TUI::set_status(const std::string& msg) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    current_status_ = msg;
    status_active_ = true;
    // An explicit status supersedes a queue-depth indicator; without this,
    // the exec thread's post-command clear_queue_indicator() would wipe a
    // status the command itself just set (e.g. /find's match position when
    // /find was queued behind a running turn).
    queue_indicator_shown_ = false;
}

void TUI::clear_status() {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    if (!status_active_) return;
    current_status_.clear();
    status_active_ = false;
    queue_indicator_shown_ = false;
}

void TUI::show_queue_depth(int pending) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d queued", pending);
    current_status_ = buf;
    status_active_ = true;
    queue_indicator_shown_ = true;
}

void TUI::clear_queue_indicator() {
    if (queue_indicator_shown_) clear_status();
}

bool TUI::queue_indicator_active() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    return queue_indicator_shown_;
}

void TUI::set_pre_input_status(const std::string& msg) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    current_pre_input_status_ = msg;
}

void TUI::clear_pre_input_status() {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    current_pre_input_status_.clear();
}

TuiChromeSnapshot TUI::chrome_snapshot() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    TuiChromeSnapshot s;
    s.rect = rect_;
    s.input_rows = input_rows_;
    s.outer_bottom = outer_bottom_;
    s.outer_top = outer_top_;
    // Mirror bottom_pad_rows() without re-entering the mutex.
    if (!outer_bottom_) {
        s.bottom_pad_rows = kCompactBottomPadRows;
    } else {
        const bool visible = footer_hint_mode_ != FooterHintMode::Hidden;
        s.bottom_pad_rows = tui_bottom_pad_rows(visible, tui_design(),
                                                /*reserve_footer_space=*/true);
    }
    s.status_active = status_active_;
    s.focus_accent = focus_accent_;
    s.footer_hint_mode = footer_hint_mode_;
    s.footer_hint_visible = footer_hint_mode_ != FooterHintMode::Hidden;
    s.status = current_status_;
    s.pre_input_status = current_pre_input_status_;
    s.activity_badge = activity_badge_;
    s.footer_override = footer_override_;
    s.footer_left_override = footer_left_override_;
    s.footer_right_override = footer_right_override_;
    return s;
}

void TUI::set_footer_hint_mode(FooterHintMode mode) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    footer_hint_mode_ = mode;
}

void TUI::set_footer_override(std::string left, std::string right) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    footer_override_ = true;
    footer_left_override_ = std::move(left);
    footer_right_override_ = std::move(right);
}

void TUI::clear_footer_override() {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    footer_override_ = false;
    footer_left_override_.clear();
    footer_right_override_.clear();
}

void TUI::set_outer_bottom(bool on_bottom) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    outer_bottom_ = on_bottom;
}

bool TUI::outer_bottom() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    return outer_bottom_;
}

void TUI::set_outer_top(bool on_top) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    outer_top_ = on_top;
}

bool TUI::outer_top() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    return outer_top_;
}

void TUI::set_focus_accent(bool active) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    focus_accent_ = active;
}

void TUI::set_activity_badge(const std::string& badge) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    activity_badge_ = badge;
}

void TUI::clear_activity_badge() {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    activity_badge_.clear();
}

// ─── ThinkingIndicator ───────────────────────────────────────────────────────

void ThinkingIndicator::start() {
    start(std::string{});
}

void ThinkingIndicator::start(const std::string& label) {
    stop();
    label_  = label;
    active_ = true;
    tick();
}

void ThinkingIndicator::stop() {
    active_ = false;
    if (tui_) tui_->clear_status();
}

void ThinkingIndicator::tick() {
    if (!active_.load() || !tui_) return;
    if (tui_->queue_indicator_active()) return;
    if (label_.empty()) {
        tui_->set_status(wait_status_label());
    } else {
        tui_->set_status(spinner_status_label(label_));
    }
}

// ─── ToolCallIndicator ───────────────────────────────────────────────────────

void ToolCallIndicator::begin() {
    armed_  = true;
    active_ = false;
    total_.store(0);
    failed_.store(0);
    inflight_.store(0);
    if (tui_) tui_->clear_pre_input_status();
}

void ToolCallIndicator::on_started() {
    if (!armed_.load()) return;
    inflight_.fetch_add(1);
    active_ = true;
    update_status();
}

void ToolCallIndicator::bump(const std::string& /*kind*/, bool ok) {
    if (!armed_.load()) return;
    total_.fetch_add(1);
    if (!ok) failed_.fetch_add(1);
    // Clamp — Started/Finished can race or skip on some paths.
    int cur = inflight_.load();
    while (cur > 0 && !inflight_.compare_exchange_weak(cur, cur - 1)) {}
    active_ = true;
    update_status();
}

void ToolCallIndicator::update_status() {
    if (!tui_ || !active_.load()) return;
    const int n = total_.load();
    if (n == 0) return;

    const int f = failed_.load();
    std::string label = std::to_string(n);
    label += " tool call";
    if (n != 1) label += "s";
    label += "\u2026";
    if (f > 0) {
        label += " (";
        label += std::to_string(f);
        label += " failed)";
    }
    tui_->set_pre_input_status(spinner_status_label(label));
}

void ToolCallIndicator::tick() {
    if (!active_.load()) return;
    update_status();
}

std::string ToolCallIndicator::finalize() {
    if (!armed_.load()) return "";
    armed_  = false;
    active_ = false;
    inflight_.store(0);
    if (tui_) tui_->clear_pre_input_status();

    const int n = total_.load();
    const int f = failed_.load();
    if (n == 0) return "";

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
    out += t.reset + "\n";
    return out;
}

} // namespace arbiter
