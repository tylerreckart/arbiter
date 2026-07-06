#pragma once

#include "repl/conversation_store.h"
#include "tui/tui.h"

#include <string>
#include <vector>

namespace arbiter {

enum class HistorySidebarKey {
    None,
    Up,
    Down,
    Enter,
    Escape,
};

struct HistorySidebarSnapshot {
    bool enabled = true;
    bool focused = false;
    int  selected = 0;
    int  scroll_offset = 0;
    std::string active_id;
    std::vector<ConversationEntry> entries;
};

// Leading (left) conversation-history sidebar state.
class HistorySidebarState {
public:
    static constexpr int kMinCols = 72;
    static constexpr int kWidth = 26;

    static int width_for_terminal(int cols, bool enabled);
    static Rect rect_for_terminal(int cols, int rows, bool enabled);

    void set_enabled(bool on, const std::string& config_dir);
    void toggle_enabled(const std::string& config_dir);
    [[nodiscard]] bool enabled() const { return enabled_; }

    void enter_focus(const ConversationStore& store, const std::string& active_id);
    void exit_focus();
    [[nodiscard]] bool focused() const { return focused_; }

    void refresh_entries(const ConversationStore& store);
    void move_selection(int delta, int visible_rows);
    [[nodiscard]] int selected_index() const { return selected_; }
    // 0 = "+ New conversation"; 1..N = entries[selected-1]
    [[nodiscard]] bool is_new_selected() const { return selected_ == 0; }
    [[nodiscard]] std::string selected_conversation_id() const;

    HistorySidebarKey handle_key(int key_byte, char csi_final = 0);
    [[nodiscard]] HistorySidebarSnapshot snapshot() const;

private:
    void clamp_selection(int visible_rows);

    bool enabled_ = true;
    bool focused_ = false;
    int  selected_ = 0;
    int  scroll_offset_ = 0;
    std::string active_id_;
    std::vector<ConversationEntry> entries_;
};

// Read one key for sidebar navigation (arrows, enter, esc).
// Returns key byte; sets csi_final for CSI sequences (A/B).
int read_history_sidebar_key(char& csi_final);

} // namespace arbiter
