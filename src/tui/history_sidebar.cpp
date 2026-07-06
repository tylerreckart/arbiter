#include "tui/history_sidebar.h"

#include "tui/tui_design.h"

#include <algorithm>
#include <unistd.h>
#include <sys/select.h>

namespace arbiter {

namespace {

int read_byte_timed(int& out, int ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    const int r = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
    if (r <= 0) return r;
    unsigned char b = 0;
    if (::read(STDIN_FILENO, &b, 1) != 1) return -1;
    out = b;
    return 1;
}

int read_byte_blocking() {
    unsigned char b = 0;
    if (::read(STDIN_FILENO, &b, 1) != 1) return -1;
    return b;
}

} // namespace

int HistorySidebarState::width_for_terminal(int cols, bool enabled) {
    if (!enabled || cols < kMinCols) return 0;
    return kWidth;
}

Rect HistorySidebarState::rect_for_terminal(int cols, int rows, bool enabled) {
    const int w = width_for_terminal(cols, enabled);
    if (w <= 0 || cols <= w || rows <= 0) return {};
    return Rect{0, 0, w, rows};
}

void HistorySidebarState::toggle_enabled(const std::string& config_dir) {
    std::lock_guard<std::mutex> lk(mu_);
    enabled_ = !enabled_;
    if (!enabled_) focused_ = false;
    set_show_history_sidebar(config_dir, enabled_);
}

bool HistorySidebarState::enabled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return enabled_;
}

bool HistorySidebarState::focused() const {
    std::lock_guard<std::mutex> lk(mu_);
    return focused_;
}

bool HistorySidebarState::is_new_selected() const {
    std::lock_guard<std::mutex> lk(mu_);
    return selected_ == 0;
}

void HistorySidebarState::set_enabled(bool on, const std::string& config_dir) {
    std::lock_guard<std::mutex> lk(mu_);
    enabled_ = on;
    if (!enabled_) focused_ = false;
    set_show_history_sidebar(config_dir, on);
}

void HistorySidebarState::enter_focus(const ConversationStore& store,
                                      const std::string& active_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_) return;
    focused_ = true;
    active_id_ = active_id;
    entries_ = store.list();
    selected_ = 0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].id == active_id_) {
            selected_ = static_cast<int>(i) + 1;
            break;
        }
    }
    scroll_offset_ = 0;
}

void HistorySidebarState::exit_focus() {
    std::lock_guard<std::mutex> lk(mu_);
    focused_ = false;
}

void HistorySidebarState::refresh_entries(const ConversationStore& store) {
    std::lock_guard<std::mutex> lk(mu_);
    entries_ = store.list();
}

void HistorySidebarState::clamp_selection(int visible_rows) {
    const int max_sel = static_cast<int>(entries_.size());
    selected_ = std::max(0, std::min(selected_, max_sel));
    if (visible_rows <= 0) return;
    if (selected_ < scroll_offset_) scroll_offset_ = selected_;
    if (selected_ >= scroll_offset_ + visible_rows) {
        scroll_offset_ = selected_ - visible_rows + 1;
    }
}

void HistorySidebarState::move_selection(int delta, int visible_rows) {
    std::lock_guard<std::mutex> lk(mu_);
    const int max_sel = static_cast<int>(entries_.size());
    selected_ = std::max(0, std::min(selected_ + delta, max_sel));
    clamp_selection(visible_rows);
}

std::string HistorySidebarState::selected_conversation_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (selected_ <= 0) return {};
    const size_t idx = static_cast<size_t>(selected_ - 1);
    if (idx >= entries_.size()) return {};
    return entries_[idx].id;
}

HistorySidebarKey HistorySidebarState::handle_key(int key_byte, char csi_final) {
    if (csi_final == 'A') return HistorySidebarKey::Up;
    if (csi_final == 'B') return HistorySidebarKey::Down;
    if (key_byte == '\r' || key_byte == '\n') return HistorySidebarKey::Enter;
    if (key_byte == 0x1B && csi_final == 0) return HistorySidebarKey::Escape;
    if (key_byte == 0x1B) return HistorySidebarKey::Escape;
    return HistorySidebarKey::None;
}

HistorySidebarSnapshot HistorySidebarState::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    HistorySidebarSnapshot s;
    s.enabled = enabled_;
    s.focused = focused_;
    s.selected = selected_;
    s.scroll_offset = scroll_offset_;
    s.active_id = active_id_;
    s.entries = entries_;
    return s;
}

int read_history_sidebar_key(char& csi_final) {
    csi_final = 0;
    int b = read_byte_blocking();
    if (b < 0) return -1;
    if (b != 0x1B) return b;

    int b2 = 0;
    if (read_byte_timed(b2, 50) <= 0) return 0x1B;

    if (b2 == '[') {
        std::string params;
        char final = 0;
        while (true) {
            int b3 = 0;
            if (read_byte_timed(b3, 50) <= 0) break;
            if ((b3 >= '0' && b3 <= '9') || b3 == ';') {
                params += static_cast<char>(b3);
                continue;
            }
            if (b3 >= 0x40 && b3 <= 0x7E) {
                final = static_cast<char>(b3);
                break;
            }
            break;
        }
        if (final) {
            csi_final = final;
            return 0x1B;
        }
        return 0x1B;
    }

    if (b2 == 'O') {
        int b3 = 0;
        if (read_byte_timed(b3, 50) > 0) {
            csi_final = static_cast<char>(b3);
            return 0x1B;
        }
    }
    return 0x1B;
}

} // namespace arbiter
