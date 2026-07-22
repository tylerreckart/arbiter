#include "tui/history_sidebar.h"

#include "tui/tui_design.h"

#include <algorithm>
#include <cctype>
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

std::string lowercase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Case-insensitive substring match on the fields the sidebar row shows:
// title and working directory.
bool entry_matches(const ConversationEntry& e, const std::string& filter_lc) {
    if (filter_lc.empty()) return true;
    return lowercase(e.title).find(filter_lc) != std::string::npos
        || lowercase(e.cwd).find(filter_lc) != std::string::npos;
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

std::vector<ConversationEntry> HistorySidebarState::visible_entries_locked() const {
    if (filter_.empty()) return entries_;
    const std::string filter_lc = lowercase(filter_);
    std::vector<ConversationEntry> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) {
        if (entry_matches(e, filter_lc)) out.push_back(e);
    }
    return out;
}

void HistorySidebarState::repin_after_filter_locked() {
    const auto visible = visible_entries_locked();
    if (!pinned_new_) {
        for (const auto& e : visible) {
            if (e.id == pinned_id_) return;   // still visible — keep it
        }
    }
    if (!visible.empty()) {
        pinned_new_ = false;
        pinned_id_ = visible.front().id;
    } else {
        pinned_new_ = true;
        pinned_id_.clear();
    }
    scroll_offset_ = 0;
}

int HistorySidebarState::index_for_pin_locked() const {
    if (pinned_new_) return 0;
    const auto visible = visible_entries_locked();
    for (size_t i = 0; i < visible.size(); ++i) {
        if (visible[i].id == pinned_id_) return static_cast<int>(i) + 1;
    }
    // Pinned conversation vanished from the list (e.g. deleted elsewhere,
    // or excluded by the filter).
    return 0;
}

void HistorySidebarState::set_pin_from_index_locked(int idx) {
    const auto visible = visible_entries_locked();
    const size_t entry_idx = static_cast<size_t>(idx - 1);
    if (idx <= 0 || entry_idx >= visible.size()) {
        pinned_new_ = true;
        pinned_id_.clear();
    } else {
        pinned_new_ = false;
        pinned_id_ = visible[entry_idx].id;
    }
}

std::string HistorySidebarState::current_title_locked() const {
    if (pinned_new_) return {};
    for (const auto& e : entries_) {
        if (e.id == pinned_id_) return e.title;
    }
    return {};
}

int HistorySidebarState::selected_index() const {
    std::lock_guard<std::mutex> lk(mu_);
    return index_for_pin_locked();
}

bool HistorySidebarState::is_new_selected() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pinned_new_;
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
    mode_ = Mode::Normal;
    rename_buffer_.clear();
    filter_.clear();
    active_id_ = active_id;
    entries_ = store.list();
    pinned_new_ = true;
    pinned_id_.clear();
    for (const auto& e : entries_) {
        if (e.id == active_id_) {
            pinned_new_ = false;
            pinned_id_ = e.id;
            break;
        }
    }
    scroll_offset_ = 0;
}

void HistorySidebarState::exit_focus() {
    std::lock_guard<std::mutex> lk(mu_);
    focused_ = false;
    mode_ = Mode::Normal;
    rename_buffer_.clear();
    filter_.clear();
}

void HistorySidebarState::refresh_entries(const ConversationStore& store) {
    std::lock_guard<std::mutex> lk(mu_);
    entries_ = store.list();
}

void HistorySidebarState::clamp_scroll_locked(int idx, int visible_rows) {
    if (visible_rows <= 0) return;
    if (idx < scroll_offset_) scroll_offset_ = idx;
    if (idx >= scroll_offset_ + visible_rows) {
        scroll_offset_ = idx - visible_rows + 1;
    }
}

void HistorySidebarState::move_selection(int delta, int visible_rows) {
    std::lock_guard<std::mutex> lk(mu_);
    const int max_sel = static_cast<int>(visible_entries_locked().size());
    int idx = std::max(0, std::min(index_for_pin_locked() + delta, max_sel));
    set_pin_from_index_locked(idx);
    clamp_scroll_locked(idx, visible_rows);
}

void HistorySidebarState::page_selection(int direction, int visible_rows) {
    const int page = std::max(1, visible_rows);
    move_selection(direction < 0 ? -page : page, visible_rows);
}

void HistorySidebarState::select_at_index(int index, int visible_rows) {
    std::lock_guard<std::mutex> lk(mu_);
    const int max_sel = static_cast<int>(visible_entries_locked().size());
    int idx = std::max(0, std::min(index, max_sel));
    set_pin_from_index_locked(idx);
    clamp_scroll_locked(idx, visible_rows);
}

int HistorySidebarState::scroll_offset() const {
    std::lock_guard<std::mutex> lk(mu_);
    return scroll_offset_;
}

int HistorySidebarState::list_row_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    // "+ New" plus every visible (filter-surviving) conversation.
    return 1 + static_cast<int>(visible_entries_locked().size());
}

bool HistorySidebarState::filter_line_visible() const {
    std::lock_guard<std::mutex> lk(mu_);
    return mode_ == Mode::Filtering || !filter_.empty();
}

std::string HistorySidebarState::selected_conversation_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pinned_new_ ? std::string{} : pinned_id_;
}

std::string HistorySidebarState::take_rename_buffer() {
    std::lock_guard<std::mutex> lk(mu_);
    std::string out = std::move(rename_buffer_);
    rename_buffer_.clear();
    return out;
}

HistorySidebarKey HistorySidebarState::handle_key(int key_byte,
                                                  char csi_final,
                                                  const std::string& csi_params) {
    std::lock_guard<std::mutex> lk(mu_);

    // SGR mouse reports must never be treated as Esc/nav in any mode.
    // The history stdin loop filters them first; this is defense in depth.
    if ((csi_final == 'M' || csi_final == 'm')
        && !csi_params.empty() && csi_params[0] == '<') {
        return HistorySidebarKey::None;
    }

    if (mode_ == Mode::Renaming) {
        if (key_byte == '\r' || key_byte == '\n') {
            mode_ = Mode::Normal;
            return HistorySidebarKey::RenameCommit;
        }
        if (key_byte == 0x1B) {
            mode_ = Mode::Normal;
            rename_buffer_.clear();
            return HistorySidebarKey::Escape;
        }
        if (key_byte == 127 || key_byte == 8) {
            if (!rename_buffer_.empty()) rename_buffer_.pop_back();
            return HistorySidebarKey::None;
        }
        if (key_byte >= 0x20 && key_byte < 0x7F) {
            rename_buffer_ += static_cast<char>(key_byte);
            return HistorySidebarKey::None;
        }
        return HistorySidebarKey::None;
    }

    if (mode_ == Mode::ConfirmDelete) {
        mode_ = Mode::Normal;
        if (key_byte == 'y' || key_byte == 'Y') return HistorySidebarKey::DeleteConfirmed;
        return HistorySidebarKey::None;
    }

    if (mode_ == Mode::Filtering) {
        // Arrows still navigate the (filtered) list while typing.
        if (csi_final == 'A') return HistorySidebarKey::Up;
        if (csi_final == 'B') return HistorySidebarKey::Down;
        if (key_byte == '\r' || key_byte == '\n') {
            // Commit: filter stays applied, keys return to normal meaning.
            mode_ = Mode::Normal;
            return HistorySidebarKey::None;
        }
        if (key_byte == 0x1B) {
            mode_ = Mode::Normal;
            filter_.clear();
            repin_after_filter_locked();
            return HistorySidebarKey::None;
        }
        if (key_byte == 127 || key_byte == 8) {
            if (!filter_.empty()) {
                filter_.pop_back();
                repin_after_filter_locked();
            }
            return HistorySidebarKey::None;
        }
        if (key_byte >= 0x20 && key_byte < 0x7F) {
            filter_ += static_cast<char>(key_byte);
            repin_after_filter_locked();
            return HistorySidebarKey::None;
        }
        return HistorySidebarKey::None;
    }

    if (csi_final == '~' && csi_params == "5") return HistorySidebarKey::PageUp;
    if (csi_final == '~' && csi_params == "6") return HistorySidebarKey::PageDown;
    if (csi_final == 'A') return HistorySidebarKey::Up;
    if (csi_final == 'B') return HistorySidebarKey::Down;
    if (key_byte == '\r' || key_byte == '\n') return HistorySidebarKey::Enter;
    if (key_byte == 0x1B) {
        // First Esc clears an applied filter; a second closes the sidebar.
        if (!filter_.empty()) {
            filter_.clear();
            repin_after_filter_locked();
            return HistorySidebarKey::None;
        }
        return HistorySidebarKey::Escape;
    }
    if (key_byte == 'k') return HistorySidebarKey::Up;
    if (key_byte == 'j') return HistorySidebarKey::Down;
    if (key_byte == 'n') return HistorySidebarKey::New;
    if (key_byte == '/') {
        mode_ = Mode::Filtering;
        return HistorySidebarKey::None;
    }

    if (key_byte == 'r') {
        if (pinned_new_) return HistorySidebarKey::None;
        mode_ = Mode::Renaming;
        rename_buffer_ = current_title_locked();
        return HistorySidebarKey::RenameStart;
    }
    if (key_byte == 'd') {
        if (pinned_new_) return HistorySidebarKey::None;
        mode_ = Mode::ConfirmDelete;
        return HistorySidebarKey::DeleteStart;
    }

    return HistorySidebarKey::None;
}

HistorySidebarSnapshot HistorySidebarState::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    HistorySidebarSnapshot s;
    s.enabled = enabled_;
    s.focused = focused_;
    s.selected = index_for_pin_locked();
    s.scroll_offset = scroll_offset_;
    s.active_id = active_id_;
    s.entries = visible_entries_locked();
    s.renaming = (mode_ == Mode::Renaming);
    s.rename_buffer = rename_buffer_;
    s.confirming_delete = (mode_ == Mode::ConfirmDelete);
    s.filtering = (mode_ == Mode::Filtering);
    s.filter = filter_;
    return s;
}

int read_history_sidebar_key(char& csi_final, std::string& csi_params) {
    csi_final = 0;
    csi_params.clear();
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
            // Include '<' so SGR mouse reports (CSI < Pb ; Px ; Py M/m)
            // tokenize here the same way PaneInputEditor does.
            if ((b3 >= '0' && b3 <= '9') || b3 == ';' || b3 == '<'
                || b3 == '?' || b3 == ':' || b3 == '>' || b3 == '=' || b3 == '$') {
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
            csi_params = params;
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
