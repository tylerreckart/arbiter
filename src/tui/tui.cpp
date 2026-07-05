// arbiter/src/tui/tui.cpp — pane layout + chrome state (OpenTUI renders pixels)

#include "tui/tui.h"
#include "cli_helpers.h"
#include "theme.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace arbiter {

void TUI::init(const std::string& /*agent*/,
               const std::string& /*model*/,
               const std::string& /*color*/) {
    rect_ = Rect{0, 0, term_cols(), term_rows()};
}

void TUI::set_rect(const Rect& r) {
    rect_ = r;
}

void TUI::resize() {
    rect_.w = term_cols();
    rect_.h = term_rows();
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
}

void TUI::clear_status() {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    if (!status_active_) return;
    current_status_.clear();
    status_active_ = false;
    queue_indicator_shown_ = false;
}

void TUI::clear_queue_indicator() {
    if (queue_indicator_shown_) clear_status();
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
    s.status_active = status_active_;
    s.focus_accent = focus_accent_;
    s.footer_hint_visible = footer_hint_visible_;
    s.status = current_status_;
    s.pre_input_status = current_pre_input_status_;
    return s;
}

void TUI::set_footer_hint_visible(bool visible) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    footer_hint_visible_ = visible;
}

void TUI::set_focus_accent(bool active) {
    std::lock_guard<std::recursive_mutex> tlk(tty_mu_);
    focus_accent_ = active;
}

// ─── Braille spinner frames (shared by both indicators) ─────────────────────

namespace {

static const char* kSpinnerFrames[] = {
    "\u2801", "\u2802", "\u2804", "\u2840", "\u2848", "\u2850",
    "\u2860", "\u28C0", "\u28C1", "\u28C2", "\u28C4", "\u28CC",
    "\u28D4", "\u28E4", "\u28E5", "\u28E6", "\u28EE", "\u28F6",
    "\u28F7", "\u28FF", "\u287F", "\u283F", "\u281F", "\u281F",
    "\u285B", "\u281B", "\u282B", "\u288B", "\u280B", "\u280D",
    "\u2809", "\u2809", "\u2811", "\u2821", "\u2881"
};
static constexpr int kSpinnerFrameCount =
    static_cast<int>(sizeof(kSpinnerFrames) / sizeof(kSpinnerFrames[0]));

int spinner_frame_index() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return static_cast<int>((ms / 80) % kSpinnerFrameCount);
}

} // namespace

// ─── ThinkingIndicator ───────────────────────────────────────────────────────

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
    const int i = spinner_frame_index();
    tui_->set_status(label_ + " " + kSpinnerFrames[i]);
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
    std::string label = kSpinnerFrames[spinner_frame_index()];
    label += " ";
    label += std::to_string(n);
    label += " tool call";
    if (n != 1) label += "s";
    label += "\u2026";
    if (f > 0) {
        label += " (";
        label += std::to_string(f);
        label += " failed)";
    }
    tui_->set_pre_input_status(label);
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
