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
    const bool visible = footer_hint_mode_ != FooterHintMode::Hidden;
    return tui_bottom_pad_rows(visible, tui_design());
}

int TUI::last_scroll_row() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    return rect_.y + rect_.h - bottom_pad_rows() - input_rows_ - kSepRows;
}

int TUI::scroll_top_row() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    return rect_.y + 1;
}

int TUI::scroll_region_rows() const {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    const int last = rect_.y + rect_.h - bottom_pad_rows() - input_rows_ - kSepRows;
    const int top  = rect_.y + 1;
    return last - top + 1;
}

void TUI::shutdown() {}

void TUI::begin_input(std::function<int()> pending_fn) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    input_rows_ = 3;

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
    needed = std::max(3, std::min(needed, kMaxInputRows));
    if (needed == input_rows_) return;
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    input_rows_ = needed;
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
    const bool visible = footer_hint_mode_ != FooterHintMode::Hidden;
    s.bottom_pad_rows = tui_bottom_pad_rows(visible, tui_design());
    s.status_active = status_active_;
    s.focus_accent = focus_accent_;
    s.footer_hint_mode = footer_hint_mode_;
    s.footer_hint_visible = visible;
    s.status = current_status_;
    s.pre_input_status = current_pre_input_status_;
    s.activity_badge = activity_badge_;
    return s;
}

void TUI::set_footer_hint_mode(FooterHintMode mode) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    footer_hint_mode_ = mode;
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
    if (tui_) tui_->clear_pre_input_status();
}

void ToolCallIndicator::bump(const std::string& /*kind*/, bool ok) {
    if (!armed_.load()) return;
    total_.fetch_add(1);
    if (!ok) failed_.fetch_add(1);
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
