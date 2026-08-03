#include "tui/history_sidebar.h"

#include "json.h"
#include "tui/opentui/kitty_key_decode.h"
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

std::string serialize_collapse(const std::unordered_set<std::string>& ids) {
    std::vector<std::string> sorted(ids.begin(), ids.end());
    std::sort(sorted.begin(), sorted.end(), [](const std::string& a, const std::string& b) {
        // Numeric order when both parse as ints; else lexicographic.
        try {
            return std::stoll(a) < std::stoll(b);
        } catch (...) {
            return a < b;
        }
    });
    JsonArray arr;
    for (const auto& id : sorted) {
        try {
            arr.push_back(jnum(static_cast<double>(std::stoll(id))));
        } catch (...) {
            // Skip non-numeric ids rather than writing invalid JSON numbers.
        }
    }
    return json_serialize(*jarr(std::move(arr)));
}

std::unordered_set<std::string> parse_collapse(const std::string& json) {
    std::unordered_set<std::string> out;
    if (json.empty()) return out;
    try {
        auto v = json_parse(json);
        if (!v || !v->is_array()) return out;
        for (const auto& item : v->as_array()) {
            if (!item) continue;
            if (item->is_number()) {
                const auto n = static_cast<std::int64_t>(item->as_number());
                if (n > 0) out.insert(std::to_string(n));
            }
        }
    } catch (...) {
        // Corrupt prefs → treat as fully expanded.
    }
    return out;
}

} // namespace

int HistorySidebarState::width_for_terminal(int cols, bool enabled) {
    if (!enabled || cols < kMinCols) return 0;
    return kWidth + kOuterGutter;
}

Rect HistorySidebarState::rect_for_terminal(int cols, int rows, bool enabled) {
    const int leading = width_for_terminal(cols, enabled);
    if (leading <= 0 || cols <= leading || rows <= 0) return {};
    return Rect{kOuterGutter, 0, kWidth, rows};
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

std::vector<HistorySidebarRow> HistorySidebarState::build_rows_locked() const {
    std::vector<HistorySidebarRow> rows;
    rows.push_back({});
    rows.back().kind = HistorySidebarRowKind::New;
    rows.back().title = "+ New conversation";

    auto push_section = [&](const char* title) {
        HistorySidebarRow r;
        r.kind = HistorySidebarRowKind::Section;
        r.title = title;
        rows.push_back(std::move(r));
    };

    auto push_conv = [&](const ConversationEntry& e, int indent) {
        HistorySidebarRow r;
        r.kind = HistorySidebarRowKind::Conversation;
        r.id = e.id;
        r.title = e.title.empty() ? "Untitled" : e.title;
        r.folder_id = e.folder_id;
        r.indent = indent;
        r.updated_at = e.updated_at;
        r.total_tokens = e.total_tokens;
        r.cwd = e.cwd;
        rows.push_back(std::move(r));
    };

    if (!folders_.empty()) {
        push_section("Folders");
        for (const auto& f : folders_) {
            std::vector<const ConversationEntry*> children;
            for (const auto& e : entries_) {
                if (e.folder_id == f.id) children.push_back(&e);
            }

            const bool collapsed = collapsed_.count(f.id) > 0;
            HistorySidebarRow header;
            header.kind = HistorySidebarRowKind::Folder;
            header.id = f.id;
            header.title = f.name;
            header.expanded = !collapsed;
            rows.push_back(std::move(header));

            if (!collapsed) {
                for (const auto* e : children) push_conv(*e, /*indent=*/1);
            }
        }
    }

    bool any_unfiled = false;
    for (const auto& e : entries_) {
        if (e.folder_id.empty()) {
            any_unfiled = true;
            break;
        }
    }
    if (any_unfiled) {
        push_section("Chats");
        for (const auto& e : entries_) {
            if (e.folder_id.empty()) push_conv(e, /*indent=*/0);
        }
    }
    return rows;
}

void HistorySidebarState::load_collapse_locked(const ConversationStore& store) {
    collapsed_ = parse_collapse(store.folder_collapse_json());
}

void HistorySidebarState::toggle_folder_locked(const std::string& folder_id) {
    if (folder_id.empty()) return;
    if (collapsed_.count(folder_id)) collapsed_.erase(folder_id);
    else collapsed_.insert(folder_id);
    ensure_pin_visible_locked();
}

void HistorySidebarState::ensure_pin_visible_locked() {
    if (pin_kind_ == PinKind::New) {
        pin_id_.clear();
        return;
    }

    auto row_matches = [&](const HistorySidebarRow& r) {
        if (pin_kind_ == PinKind::Folder)
            return r.kind == HistorySidebarRowKind::Folder && r.id == pin_id_;
        return r.kind == HistorySidebarRowKind::Conversation && r.id == pin_id_;
    };

    const auto rows = build_rows_locked();
    for (const auto& r : rows) {
        if (row_matches(r)) return;
    }

    // Conversation still exists but is hidden under a collapsed folder —
    // pin the folder so highlight / new-chat target stay coherent.
    if (pin_kind_ == PinKind::Conversation) {
        for (const auto& e : entries_) {
            if (e.id != pin_id_) continue;
            if (!e.folder_id.empty() && collapsed_.count(e.folder_id) > 0) {
                pin_kind_ = PinKind::Folder;
                pin_id_ = e.folder_id;
                return;
            }
            break;
        }
    }

    pin_kind_ = PinKind::New;
    pin_id_.clear();
}

int HistorySidebarState::index_for_pin_locked() const {
    if (pin_kind_ == PinKind::New) return 0;
    const auto rows = build_rows_locked();
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (pin_kind_ == PinKind::Folder
            && r.kind == HistorySidebarRowKind::Folder
            && r.id == pin_id_) {
            return static_cast<int>(i);
        }
        if (pin_kind_ == PinKind::Conversation
            && r.kind == HistorySidebarRowKind::Conversation
            && r.id == pin_id_) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

void HistorySidebarState::set_pin_from_index_locked(int idx) {
    const auto rows = build_rows_locked();
    if (rows.empty()) {
        pin_kind_ = PinKind::New;
        pin_id_.clear();
        return;
    }
    const int n = static_cast<int>(rows.size());
    idx = std::max(0, std::min(idx, n - 1));

    // Section headers are not selectable — snap to the nearest real row.
    auto selectable = [](const HistorySidebarRow& r) {
        return r.kind != HistorySidebarRowKind::Section;
    };
    if (!selectable(rows[static_cast<size_t>(idx)])) {
        int found = -1;
        for (int i = idx + 1; i < n; ++i) {
            if (selectable(rows[static_cast<size_t>(i)])) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            for (int i = idx - 1; i >= 0; --i) {
                if (selectable(rows[static_cast<size_t>(i)])) {
                    found = i;
                    break;
                }
            }
        }
        if (found < 0) {
            pin_kind_ = PinKind::New;
            pin_id_.clear();
            return;
        }
        idx = found;
    }

    const auto& r = rows[static_cast<size_t>(idx)];
    if (r.kind == HistorySidebarRowKind::Folder) {
        pin_kind_ = PinKind::Folder;
        pin_id_ = r.id;
    } else if (r.kind == HistorySidebarRowKind::Conversation) {
        pin_kind_ = PinKind::Conversation;
        pin_id_ = r.id;
    } else {
        pin_kind_ = PinKind::New;
        pin_id_.clear();
    }
}

std::string HistorySidebarState::current_title_locked() const {
    if (pin_kind_ == PinKind::New) return {};
    if (pin_kind_ == PinKind::Folder) {
        for (const auto& f : folders_) {
            if (f.id == pin_id_) return f.name;
        }
        return {};
    }
    for (const auto& e : entries_) {
        if (e.id == pin_id_) return e.title;
    }
    return {};
}

int HistorySidebarState::selected_index() const {
    std::lock_guard<std::mutex> lk(mu_);
    return index_for_pin_locked();
}

bool HistorySidebarState::is_new_selected() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pin_kind_ == PinKind::New;
}

bool HistorySidebarState::is_folder_selected() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pin_kind_ == PinKind::Folder;
}

std::string HistorySidebarState::selected_conversation_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pin_kind_ == PinKind::Conversation ? pin_id_ : std::string{};
}

std::string HistorySidebarState::selected_folder_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pin_kind_ == PinKind::Folder ? pin_id_ : std::string{};
}

std::string HistorySidebarState::new_target_folder_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (pin_kind_ == PinKind::Folder) return pin_id_;
    if (pin_kind_ == PinKind::Conversation) {
        for (const auto& e : entries_) {
            if (e.id == pin_id_) return e.folder_id;
        }
    }
    return {};
}

std::string HistorySidebarState::collapse_json() const {
    std::lock_guard<std::mutex> lk(mu_);
    return serialize_collapse(collapsed_);
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
    creating_folder_ = false;
    rename_target_id_.clear();
    rename_target_is_folder_ = false;
    menu_index_ = 0;
    move_index_ = 0;
    move_labels_.clear();
    move_folder_ids_.clear();
    move_result_.clear();
    active_id_ = active_id;
    entries_ = store.list();
    folders_ = store.list_folders();
    load_collapse_locked(store);
    pin_kind_ = PinKind::New;
    pin_id_.clear();
    for (const auto& e : entries_) {
        if (e.id == active_id_) {
            pin_kind_ = PinKind::Conversation;
            pin_id_ = e.id;
            break;
        }
    }
    ensure_pin_visible_locked();
    scroll_offset_ = 0;
}

void HistorySidebarState::exit_focus() {
    std::lock_guard<std::mutex> lk(mu_);
    focused_ = false;
    mode_ = Mode::Normal;
    rename_buffer_.clear();
    creating_folder_ = false;
    rename_target_id_.clear();
    rename_target_is_folder_ = false;
    menu_index_ = 0;
    move_index_ = 0;
    move_labels_.clear();
    move_folder_ids_.clear();
    move_result_.clear();
    move_from_folder_id_.clear();
}

void HistorySidebarState::begin_move_locked() {
    move_labels_.clear();
    move_folder_ids_.clear();
    move_index_ = 0;
    move_from_folder_id_.clear();
    for (const auto& e : entries_) {
        if (e.id == pin_id_) {
            move_from_folder_id_ = e.folder_id;
            break;
        }
    }
    for (const auto& f : folders_) {
        move_labels_.push_back(f.name);
        move_folder_ids_.push_back(f.id);
    }
    move_labels_.push_back("Unfiled");
    move_folder_ids_.push_back({});
    for (size_t i = 0; i < move_folder_ids_.size(); ++i) {
        if (move_folder_ids_[i] == move_from_folder_id_) {
            move_index_ = static_cast<int>(i);
            break;
        }
    }
    mode_ = Mode::Moving;
}

HistorySidebarKey HistorySidebarState::begin_new_folder_locked() {
    creating_folder_ = true;
    rename_target_id_.clear();
    rename_target_is_folder_ = false;
    mode_ = Mode::Renaming;
    rename_buffer_.clear();
    return HistorySidebarKey::RenameStart;
}

HistorySidebarKey HistorySidebarState::begin_rename_locked() {
    if (pin_kind_ == PinKind::New) return HistorySidebarKey::None;
    creating_folder_ = false;
    rename_target_id_ = pin_id_;
    rename_target_is_folder_ = (pin_kind_ == PinKind::Folder);
    mode_ = Mode::Renaming;
    rename_buffer_ = current_title_locked();
    return HistorySidebarKey::RenameStart;
}

HistorySidebarKey HistorySidebarState::commit_menu_locked() {
    const bool folder = (pin_kind_ == PinKind::Folder);
    const bool is_new = (pin_kind_ == PinKind::New);
    const int pick = menu_index_;
    menu_index_ = 0;
    mode_ = Mode::Normal;

    if (is_new) {
        if (pick == kNewMenuFolder) return begin_new_folder_locked();
        return HistorySidebarKey::None;
    }

    if (folder) {
        if (pick == kFoldMenuRename) return begin_rename_locked();
        if (pick == kFoldMenuNew) return HistorySidebarKey::New;
        if (pick == kFoldMenuDelete) {
            mode_ = Mode::ConfirmDelete;
            return HistorySidebarKey::DeleteStart;
        }
        return HistorySidebarKey::None;
    }

    if (pick == kConvMenuRename) return begin_rename_locked();
    if (pick == kConvMenuMove) {
        begin_move_locked();
        return HistorySidebarKey::MoveStart;
    }
    if (pick == kConvMenuDelete) {
        mode_ = Mode::ConfirmDelete;
        return HistorySidebarKey::DeleteStart;
    }
    return HistorySidebarKey::Enter;
}

void HistorySidebarState::refresh_entries(const ConversationStore& store) {
    std::lock_guard<std::mutex> lk(mu_);
    entries_ = store.list();
    folders_ = store.list_folders();
    load_collapse_locked(store);
    ensure_pin_visible_locked();
    const int n = static_cast<int>(build_rows_locked().size());
    scroll_offset_ = std::max(0, std::min(scroll_offset_, std::max(0, n - 1)));
}

void HistorySidebarState::clamp_scroll_locked(int idx, int visible_lines) {
    if (visible_lines <= 0) return;
    const auto rows = build_rows_locked();
    const int n = static_cast<int>(rows.size());
    if (n <= 0) {
        scroll_offset_ = 0;
        return;
    }
    idx = std::max(0, std::min(idx, n - 1));
    scroll_offset_ = std::max(0, std::min(scroll_offset_, n - 1));

    auto span = [&](int i) {
        const auto& r = rows[static_cast<size_t>(i)];
        return history_sidebar_gap_before(r.kind, i)
            + history_sidebar_row_height(r.kind);
    };
    auto lines_through = [&](int from, int to) {
        int lines = 0;
        for (int i = from; i <= to; ++i) lines += span(i);
        return lines;
    };

    if (idx < scroll_offset_) {
        scroll_offset_ = idx;
        return;
    }
    // Scroll forward until the selected row fully fits in the line budget.
    while (scroll_offset_ < idx
           && lines_through(scroll_offset_, idx) > visible_lines) {
        ++scroll_offset_;
    }
}

void HistorySidebarState::move_selection(int delta, int visible_lines) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto rows = build_rows_locked();
    const int n = static_cast<int>(rows.size());
    if (n <= 0) return;
    const int step = delta < 0 ? -1 : 1;
    int idx = index_for_pin_locked();
    for (int guard = 0; guard < n; ++guard) {
        idx = std::max(0, std::min(idx + step, n - 1));
        if (rows[static_cast<size_t>(idx)].kind != HistorySidebarRowKind::Section) {
            break;
        }
        // Hit the edge while still on a section — stop.
        if ((step < 0 && idx == 0) || (step > 0 && idx == n - 1)) break;
    }
    set_pin_from_index_locked(idx);
    clamp_scroll_locked(index_for_pin_locked(), visible_lines);
}

void HistorySidebarState::page_selection(int direction, int visible_lines) {
    int page = 1;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto rows = build_rows_locked();
        const int n = static_cast<int>(rows.size());
        int lines = 0;
        int fitted = 0;
        for (int i = scroll_offset_; i < n; ++i) {
            const auto& r = rows[static_cast<size_t>(i)];
            const int need = history_sidebar_gap_before(r.kind, i)
                + history_sidebar_row_height(r.kind);
            if (fitted > 0 && lines + need > visible_lines) break;
            lines += need;
            ++fitted;
        }
        page = std::max(1, fitted);
    }
    move_selection(direction < 0 ? -page : page, visible_lines);
}

void HistorySidebarState::select_at_index(int index, int visible_rows) {
    std::lock_guard<std::mutex> lk(mu_);
    const int max_sel = std::max(0, static_cast<int>(build_rows_locked().size()) - 1);
    int idx = std::max(0, std::min(index, max_sel));
    set_pin_from_index_locked(idx);
    clamp_scroll_locked(index_for_pin_locked(), visible_rows);
}

void HistorySidebarState::select_folder(const std::string& folder_id,
                                        int visible_rows) {
    std::lock_guard<std::mutex> lk(mu_);
    if (folder_id.empty()) return;
    pin_kind_ = PinKind::Folder;
    pin_id_ = folder_id;
    clamp_scroll_locked(index_for_pin_locked(), visible_rows);
}

int HistorySidebarState::scroll_offset() const {
    std::lock_guard<std::mutex> lk(mu_);
    return scroll_offset_;
}

int HistorySidebarState::list_row_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(build_rows_locked().size());
}

std::string HistorySidebarState::take_rename_buffer() {
    std::lock_guard<std::mutex> lk(mu_);
    creating_folder_ = false;
    rename_target_id_.clear();
    rename_target_is_folder_ = false;
    std::string out = std::move(rename_buffer_);
    rename_buffer_.clear();
    return out;
}

bool HistorySidebarState::is_creating_folder() const {
    std::lock_guard<std::mutex> lk(mu_);
    return creating_folder_;
}

std::string HistorySidebarState::rename_target_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return rename_target_id_;
}

bool HistorySidebarState::rename_target_is_folder() const {
    std::lock_guard<std::mutex> lk(mu_);
    return rename_target_is_folder_;
}

std::string HistorySidebarState::take_move_folder_id() {
    std::lock_guard<std::mutex> lk(mu_);
    std::string out = std::move(move_result_);
    move_result_.clear();
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

    // Kitty keyboard protocol (disambiguate) re-encodes Enter/Esc/Backspace
    // and ctrl keys as CSI-u. Without decoding, Enter arrives as Esc+u and
    // cancels rename / closes the sidebar instead of committing.
    if (key_byte == 0x1B && csi_final == 'u') {
        if (auto legacy = opentui::decode_kitty_csi_u(csi_params)) {
            key_byte = *legacy;
            csi_final = 0;
        } else {
            return HistorySidebarKey::None;
        }
    }

    if (mode_ == Mode::Renaming) {
        if (key_byte == '\r' || key_byte == '\n') {
            // Empty names are not useful — keep editing until Esc or a
            // non-blank title (trim trailing spaces for the emptiness check).
            std::string trimmed = rename_buffer_;
            while (!trimmed.empty() &&
                   (trimmed.back() == ' ' || trimmed.back() == '\t'))
                trimmed.pop_back();
            if (trimmed.empty()) return HistorySidebarKey::None;
            rename_buffer_ = std::move(trimmed);
            mode_ = Mode::Normal;
            return HistorySidebarKey::RenameCommit;
        }
        if (key_byte == 0x1B) {
            // Only a bare Esc (or kitty-decoded Esc → csi_final cleared)
            // cancels. Async terminal replies (cursor position, DA, etc.)
            // and arrows also arrive as Esc+CSI — treating them as cancel
            // aborts rename the moment the user tries to type.
            if (csi_final != 0) return HistorySidebarKey::None;
            mode_ = Mode::Normal;
            rename_buffer_.clear();
            creating_folder_ = false;
            rename_target_id_.clear();
            rename_target_is_folder_ = false;
            // Stay focused — a second Esc leaves the sidebar.
            return HistorySidebarKey::None;
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

    if (mode_ == Mode::Moving) {
        const int count = static_cast<int>(move_labels_.size());
        if (count <= 0) {
            mode_ = Mode::Normal;
            return HistorySidebarKey::None;
        }
        if (csi_final == 'A' || key_byte == 'k') {
            move_index_ = (move_index_ + count - 1) % count;
            return HistorySidebarKey::None;
        }
        if (csi_final == 'B' || key_byte == 'j') {
            move_index_ = (move_index_ + 1) % count;
            return HistorySidebarKey::None;
        }
        if (key_byte == '\r' || key_byte == '\n') {
            if (move_index_ >= 0 && move_index_ < count) {
                move_result_ = move_folder_ids_[static_cast<size_t>(move_index_)];
            }
            mode_ = Mode::Normal;
            move_index_ = 0;
            move_labels_.clear();
            move_folder_ids_.clear();
            move_from_folder_id_.clear();
            return HistorySidebarKey::MoveCommit;
        }
        if (key_byte == 0x1B) {
            mode_ = Mode::Normal;
            move_index_ = 0;
            move_labels_.clear();
            move_folder_ids_.clear();
            move_from_folder_id_.clear();
            move_result_.clear();
            return HistorySidebarKey::None;
        }
        return HistorySidebarKey::None;
    }

    if (mode_ == Mode::Menu) {
        const bool folder = (pin_kind_ == PinKind::Folder);
        const bool is_new = (pin_kind_ == PinKind::New);
        const int count = is_new ? kNewMenuCount
                        : folder ? kFoldMenuCount
                                 : kConvMenuCount;
        if (csi_final == 'A' || key_byte == 'k') {
            menu_index_ = (menu_index_ + count - 1) % count;
            return HistorySidebarKey::None;
        }
        if (csi_final == 'B' || key_byte == 'j') {
            menu_index_ = (menu_index_ + 1) % count;
            return HistorySidebarKey::None;
        }
        if (key_byte == '\r' || key_byte == '\n') {
            return commit_menu_locked();
        }
        if (key_byte == 0x1B || key_byte == 'm') {
            mode_ = Mode::Normal;
            menu_index_ = 0;
            return HistorySidebarKey::None;
        }
        if (is_new) {
            if (key_byte == 'f' || key_byte == 'F') {
                menu_index_ = kNewMenuFolder;
                return commit_menu_locked();
            }
        } else if (folder) {
            if (key_byte == 'r' || key_byte == 'R') {
                menu_index_ = kFoldMenuRename;
                return commit_menu_locked();
            }
            if (key_byte == 'n' || key_byte == 'N') {
                menu_index_ = kFoldMenuNew;
                return commit_menu_locked();
            }
            if (key_byte == 'd' || key_byte == 'D') {
                menu_index_ = kFoldMenuDelete;
                return commit_menu_locked();
            }
        } else {
            if (key_byte == 'o' || key_byte == 'O') {
                menu_index_ = kConvMenuOpen;
                return commit_menu_locked();
            }
            if (key_byte == 'r' || key_byte == 'R') {
                menu_index_ = kConvMenuRename;
                return commit_menu_locked();
            }
            if (key_byte == 'v' || key_byte == 'V'
                || key_byte == 't' || key_byte == 'T') {
                menu_index_ = kConvMenuMove;
                return commit_menu_locked();
            }
            if (key_byte == 'd' || key_byte == 'D') {
                menu_index_ = kConvMenuDelete;
                return commit_menu_locked();
            }
        }
        return HistorySidebarKey::None;
    }

    if (csi_final == '~' && csi_params == "5") return HistorySidebarKey::PageUp;
    if (csi_final == '~' && csi_params == "6") return HistorySidebarKey::PageDown;
    if (csi_final == 'A') return HistorySidebarKey::Up;
    if (csi_final == 'B') return HistorySidebarKey::Down;
    if (key_byte == '\r' || key_byte == '\n') {
        if (pin_kind_ == PinKind::Folder) {
            toggle_folder_locked(pin_id_);
            return HistorySidebarKey::ToggleFolder;
        }
        return HistorySidebarKey::Enter;
    }
    if (key_byte == 0x1B) return HistorySidebarKey::Escape;
    if (key_byte == 'k') return HistorySidebarKey::Up;
    if (key_byte == 'j') return HistorySidebarKey::Down;
    if (key_byte == 'n') return HistorySidebarKey::New;
    if (key_byte == 'f') return begin_new_folder_locked();

    if (key_byte == 'r') return begin_rename_locked();
    if (key_byte == 'd') {
        if (pin_kind_ == PinKind::New) return HistorySidebarKey::None;
        mode_ = Mode::ConfirmDelete;
        return HistorySidebarKey::DeleteStart;
    }
    if (key_byte == 'm') {
        mode_ = Mode::Menu;
        if (pin_kind_ == PinKind::New) menu_index_ = kNewMenuFolder;
        else if (pin_kind_ == PinKind::Folder) menu_index_ = kFoldMenuRename;
        else menu_index_ = kConvMenuOpen;
        return HistorySidebarKey::MenuOpen;
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
    s.rows = build_rows_locked();
    s.entries = entries_;
    s.renaming = (mode_ == Mode::Renaming);
    s.rename_buffer = rename_buffer_;
    s.rename_is_folder = (mode_ == Mode::Renaming && pin_kind_ == PinKind::Folder
                          && !creating_folder_);
    s.creating_folder = (mode_ == Mode::Renaming && creating_folder_);
    s.confirming_delete = (mode_ == Mode::ConfirmDelete);
    s.delete_is_folder = (mode_ == Mode::ConfirmDelete && pin_kind_ == PinKind::Folder);
    s.menu_open = (mode_ == Mode::Menu);
    s.menu_index = menu_index_;
    s.menu_is_folder = (pin_kind_ == PinKind::Folder);
    s.menu_is_new = (pin_kind_ == PinKind::New);
    s.moving = (mode_ == Mode::Moving);
    s.move_index = move_index_;
    s.move_labels = move_labels_;
    s.move_is_current.clear();
    s.move_is_current.reserve(move_folder_ids_.size());
    for (const auto& fid : move_folder_ids_) {
        s.move_is_current.push_back(fid == move_from_folder_id_);
    }
    return s;
}

int read_history_sidebar_key(char& csi_final, std::string& csi_params) {
    // Loop so kitty key-release reports (decode → nullopt) and async
    // terminal capability replies are skipped rather than surfacing as a
    // bare Esc to the caller (which would abort rename / close the sidebar).
    for (;;) {
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
            if (!final) return 0x1B;

            // Swallow terminal replies that OpenTUI's handshake / render
            // tick provoke (same set PaneInputEditor ignores).
            if (final == 'R') continue;  // cursor position
            if (final == 'c' && !params.empty()
                && (params[0] == '?' || params[0] == '>' || params[0] == '=')) {
                continue;  // DA / device attributes
            }
            if (final == 'y' && params.find('$') != std::string::npos) continue;
            if (final == 'u' && !params.empty() && params[0] == '?') continue;

            if (final == 'u') {
                if (auto legacy = opentui::decode_kitty_csi_u(params)) return *legacy;
                continue;  // release / unmapped — read next key
            }
            csi_final = final;
            csi_params = params;
            return 0x1B;
        }

        if (b2 == ']') {
            // OSC reply — discard until ST / BEL.
            for (;;) {
                int b3 = 0;
                if (read_byte_timed(b3, 100) <= 0) break;
                if (b3 == 0x07 || b3 == 0x9C) break;
                if (b3 == 0x1B) {
                    int b4 = 0;
                    if (read_byte_timed(b4, 10) > 0 && b4 == '\\') break;
                }
            }
            continue;
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
}

} // namespace arbiter
