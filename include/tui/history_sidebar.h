#pragma once

#include "repl/conversation_store.h"
#include "tui/tui.h"

#include <mutex>
#include <string>
#include <vector>

namespace arbiter {

enum class HistorySidebarKey {
    None,
    Up,
    Down,
    Enter,
    Escape,
    New,
    // Enter rename mode for the selected entry (not "+ New"). Character
    // input, backspace, commit (Enter), and cancel (Esc) while renaming are
    // handled internally by handle_key and never surface as their own key —
    // callers only see RenameCommit, at which point take_rename_buffer()
    // returns the text to persist.
    RenameStart,
    RenameCommit,
    // Enter soft-delete confirm mode for the selected entry. 'y'/'Y' while
    // confirming surfaces as DeleteConfirmed; anything else cancels
    // silently (handled internally, surfaces as None).
    DeleteStart,
    DeleteConfirmed,
    PageUp,
    PageDown,
};

struct HistorySidebarSnapshot {
    bool enabled = true;
    bool focused = false;
    int  selected = 0;
    int  scroll_offset = 0;
    std::string active_id;
    // Entries surviving the active filter (all entries when no filter).
    std::vector<ConversationEntry> entries;
    // Inline edit/confirm state for the frame drawer to render on the
    // selected row instead of its normal title/subtitle.
    bool renaming = false;
    std::string rename_buffer;
    bool confirming_delete = false;
    // Type-to-filter state: `filtering` while the filter line is being
    // edited ('/'), `filter` is the applied text (persists after Enter
    // commits the filter until Esc clears it or focus is re-entered).
    bool filtering = false;
    std::string filter;
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
    [[nodiscard]] bool enabled() const;
    void enter_focus(const ConversationStore& store, const std::string& active_id);
    void exit_focus();
    [[nodiscard]] bool focused() const;

    void refresh_entries(const ConversationStore& store);
    void move_selection(int delta, int visible_rows);
    // PgUp (direction < 0) / PgDn (direction > 0): move a full page.
    void page_selection(int direction, int visible_rows);
    // Absolute list-row index (0 = "+ New"); clamps and updates scroll.
    void select_at_index(int index, int visible_rows);
    // Index into the drawn row list: 0 = "+ New conversation"; 1..N =
    // entries[selected-1]. Recomputed on every call from the id-pinned
    // selection so it stays correct even if entries_ was re-sorted since
    // the last move (selection is pinned by conversation id, not index).
    [[nodiscard]] int selected_index() const;
    [[nodiscard]] bool is_new_selected() const;
    [[nodiscard]] std::string selected_conversation_id() const;
    [[nodiscard]] int scroll_offset() const;
    // Painted list length including the leading "+ New" row.
    [[nodiscard]] int list_row_count() const;
    // True while the filter line is painted (editing '/' or a committed
    // filter is still narrowing the list). Shifts the conversation rows
    // down by one; mouse hit-testing and visible-row math must match.
    [[nodiscard]] bool filter_line_visible() const;

    // Valid only immediately after handle_key() returns RenameCommit —
    // returns the edited text and clears the internal buffer.
    [[nodiscard]] std::string take_rename_buffer();

    HistorySidebarKey handle_key(int key_byte, char csi_final = 0, const std::string& csi_params = {});
    [[nodiscard]] HistorySidebarSnapshot snapshot() const;

private:
    enum class Mode { Normal, Renaming, ConfirmDelete, Filtering };

    // Assumes mu_ is already held by the caller.
    int index_for_pin_locked() const;
    void set_pin_from_index_locked(int idx);
    void clamp_scroll_locked(int idx, int visible_rows);
    std::string current_title_locked() const;
    // Entries surviving filter_ (all of entries_ when filter_ is empty).
    std::vector<ConversationEntry> visible_entries_locked() const;
    // Re-pin after a filter edit: keep the pinned id if it's still
    // visible, otherwise pin the first visible entry (or "+ New").
    void repin_after_filter_locked();

    mutable std::mutex mu_;
    bool enabled_ = true;
    bool focused_ = false;
    // The selection is pinned by conversation id so a background re-sort
    // (e.g. autosave bumping updated_at) never silently moves the cursor
    // to a different conversation.
    bool pinned_new_ = true;
    std::string pinned_id_;
    int  scroll_offset_ = 0;
    std::string active_id_;
    std::vector<ConversationEntry> entries_;

    Mode mode_ = Mode::Normal;
    std::string rename_buffer_;
    std::string filter_;
};

// Read one key for sidebar navigation (arrows, enter, esc, PgUp/PgDn).
// Returns key byte; sets csi_final for CSI sequences (A/B/~) and csi_params
// to the numeric parameter string (e.g. "5" for PgUp's ESC[5~).
int read_history_sidebar_key(char& csi_final, std::string& csi_params);

} // namespace arbiter
