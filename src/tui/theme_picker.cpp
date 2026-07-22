#include "tui/theme_picker.h"

#include <algorithm>

namespace arbiter {

void ThemePickerState::open(std::vector<std::string> themes,
                            std::string_view active_name) {
    std::lock_guard<std::mutex> lk(mu_);
    themes_ = std::move(themes);
    selected_ = 0;
    scroll_offset_ = 0;
    if (!active_name.empty()) {
        for (size_t i = 0; i < themes_.size(); ++i) {
            if (themes_[i] == active_name) {
                selected_ = static_cast<int>(i);
                break;
            }
        }
    }
    active_ = !themes_.empty();
}

void ThemePickerState::close() {
    std::lock_guard<std::mutex> lk(mu_);
    active_ = false;
    themes_.clear();
    selected_ = 0;
    scroll_offset_ = 0;
}

bool ThemePickerState::active() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_;
}

void ThemePickerState::clamp_scroll_locked(int visible_rows) {
    const int n = static_cast<int>(themes_.size());
    if (visible_rows <= 0 || n <= 0) {
        scroll_offset_ = 0;
        return;
    }
    if (selected_ < scroll_offset_) scroll_offset_ = selected_;
    if (selected_ >= scroll_offset_ + visible_rows) {
        scroll_offset_ = selected_ - visible_rows + 1;
    }
    const int max_off = std::max(0, n - visible_rows);
    scroll_offset_ = std::clamp(scroll_offset_, 0, max_off);
}

void ThemePickerState::move_selection(int delta, int visible_rows) {
    std::lock_guard<std::mutex> lk(mu_);
    const int n = static_cast<int>(themes_.size());
    if (!active_ || n == 0) return;
    selected_ = (selected_ + delta) % n;
    if (selected_ < 0) selected_ += n;
    clamp_scroll_locked(visible_rows);
}

void ThemePickerState::page_selection(int direction, int visible_rows) {
    std::lock_guard<std::mutex> lk(mu_);
    const int n = static_cast<int>(themes_.size());
    if (!active_ || n == 0 || visible_rows <= 0) return;
    const int step = std::max(1, visible_rows - 1);
    if (direction < 0) selected_ = std::max(0, selected_ - step);
    else selected_ = std::min(n - 1, selected_ + step);
    clamp_scroll_locked(visible_rows);
}

std::string ThemePickerState::selected_theme() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!active_ || themes_.empty()) return {};
    return themes_[static_cast<size_t>(selected_)];
}

int ThemePickerState::selected_index() const {
    std::lock_guard<std::mutex> lk(mu_);
    return selected_;
}

ThemePickerSnapshot ThemePickerState::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    ThemePickerSnapshot s;
    s.active = active_;
    s.selected = selected_;
    s.scroll_offset = scroll_offset_;
    s.themes = themes_;
    return s;
}

} // namespace arbiter
