#pragma once

#include "repl/conversation_store.h"
#include "tui/tui.h"

#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace arbiter {

enum class HistorySidebarKey {
    None,
    Up,
    Down,
    Enter,
    Escape,
    New,
    // Enter rename mode for the selected entry (not "+ New"), or open
    // new-folder name entry (`f` / New-row menu; drawn as a modal).
    // Character input, backspace, commit (Enter), and cancel (Esc) while
    // renaming are handled internally — callers only see RenameCommit, at
    // which point take_rename_buffer() returns the text and
    // is_creating_folder() distinguishes create vs rename.
    RenameStart,
    RenameCommit,
    // Enter soft-delete confirm mode for the selected entry. 'y'/'Y' while
    // confirming surfaces as DeleteConfirmed; anything else cancels
    // silently (handled internally, surfaces as None).
    DeleteStart,
    DeleteConfirmed,
    // Opened the per-row action menu ('m'); callers redraw. Navigating and
    // cancelling the menu are handled internally (None); committing an
    // item surfaces as Enter / RenameStart / DeleteStart / New / MoveStart.
    MenuOpen,
    // Enter on a folder header toggled collapse; callers should persist
    // collapse_json() via ConversationStore::set_folder_collapse_json.
    ToggleFolder,
    // Move-to picker opened (conversation menu → Move to…). Navigating
    // and cancelling are internal; Enter surfaces MoveCommit and
    // take_move_folder_id() returns the target (empty = unfiled).
    MoveStart,
    MoveCommit,
    PageUp,
    PageDown,
};

enum class HistorySidebarRowKind {
    New,
    Section,  // Non-selectable category header ("Folders" / "Chats").
    Folder,
    Conversation,
};

// Painted height in terminal rows. Conversations use two lines (title +
// meta); Section headers are divider + blank; New / Folder are single-line.
// Editing/confirm overlays temporarily force height 2 in the frame drawer.
inline int history_sidebar_row_height(HistorySidebarRowKind kind) {
    using K = HistorySidebarRowKind;
    if (kind == K::Conversation || kind == K::Section) return 2;
    return 1;
}

// Blank lines inserted above a row (Session sidebar leaves a gap before
// each new section). Keep draw + mouse hit in sync.
inline int history_sidebar_gap_before(HistorySidebarRowKind kind, int abs_index) {
    if (kind == HistorySidebarRowKind::Section && abs_index > 0) return 1;
    return 0;
}

// One painted list row in the history sidebar (flattened folder tree).
struct HistorySidebarRow {
    HistorySidebarRowKind kind = HistorySidebarRowKind::Conversation;
    // Conversation id, folder id, or empty for "+ New" / section headers.
    std::string id;
    std::string title;
    // Parent folder for Conversation rows (empty = unfiled).
    std::string folder_id;
    // Folder rows only: true → [-] (children shown), false → [+].
    bool expanded = true;
    int indent = 0;
    // Conversation subtitle fields (frame formats relative time / tokens).
    std::int64_t updated_at = 0;
    int total_tokens = 0;
    std::string cwd;
};

struct HistorySidebarSnapshot {
    bool enabled = true;
    bool focused = false;
    int  selected = 0;
    int  scroll_offset = 0;
    std::string active_id;
    // Flattened tree rows for the frame (+ New, folders, conversations).
    std::vector<HistorySidebarRow> rows;
    // All conversations (same as ConversationStore::list()).
    std::vector<ConversationEntry> entries;
    // Inline edit/confirm/menu state for the frame drawer to render on the
    // selected row instead of its normal title/subtitle.
    bool renaming = false;
    std::string rename_buffer;
    // True while renaming a folder (vs a conversation title).
    bool rename_is_folder = false;
    // True while naming a brand-new folder (`f` / New-row menu).
    bool creating_folder = false;
    bool confirming_delete = false;
    // True while confirming deletion of a folder (vs a conversation).
    bool delete_is_folder = false;
    // Per-row action menu ('m'). Labels depend on folder / conversation / New.
    bool menu_open = false;
    int  menu_index = 0;
    bool menu_is_folder = false;
    bool menu_is_new = false;
    // Move-to picker overlay (folders + "Unfiled").
    bool moving = false;
    int  move_index = 0;
    std::vector<std::string> move_labels;
    // Parallel to move_labels: true when this destination is the chat's
    // current folder (drawn with a checkmark).
    std::vector<bool> move_is_current;
};

// Leading (left) conversation-history sidebar state.
class HistorySidebarState {
public:
    static constexpr int kMinCols = 72;
    static constexpr int kWidth = 26;
    // Blank column between the terminal edge and the conversations box so
    // outer spacing matches the pane's edge pad beside the box.
    static constexpr int kOuterGutter = 1;

    // Columns reserved on the left (box width + outer gutter), or 0 when hidden.
    static int width_for_terminal(int cols, bool enabled);
    // Drawn box rect (excludes the leading gutter column).
    static Rect rect_for_terminal(int cols, int rows, bool enabled);

    void set_enabled(bool on, const std::string& config_dir);
    void toggle_enabled(const std::string& config_dir);
    [[nodiscard]] bool enabled() const;
    void enter_focus(const ConversationStore& store, const std::string& active_id);
    void enter_focus_list(std::vector<ConversationEntry> entries,
                          const std::string& active_id);
    void exit_focus();
    [[nodiscard]] bool focused() const;

    void refresh_entries(const ConversationStore& store);
    // Remote TUI: feed an in-memory conversation list (no local store).
    void refresh_entries_list(std::vector<ConversationEntry> entries,
                              const std::string& active_id);
    void move_selection(int delta, int visible_rows);
    // PgUp (direction < 0) / PgDn (direction > 0): move a full page.
    void page_selection(int direction, int visible_rows);
    // Absolute list-row index (0 = "+ New"); clamps and updates scroll.
    void select_at_index(int index, int visible_rows);
    // Pin selection to a folder id (e.g. after creating one).
    void select_folder(const std::string& folder_id, int visible_rows);
    // Index into the drawn row list: 0 = "+ New"; remainder follows the
    // flattened folder tree. Recomputed from the id-pinned selection so
    // it stays correct across re-sorts / collapse toggles.
    [[nodiscard]] int selected_index() const;
    [[nodiscard]] bool is_new_selected() const;
    [[nodiscard]] bool is_folder_selected() const;
    [[nodiscard]] std::string selected_conversation_id() const;
    [[nodiscard]] std::string selected_folder_id() const;
    // Folder to file a new conversation into: selection's folder (header
    // or child), else empty (unfiled).
    [[nodiscard]] std::string new_target_folder_id() const;
    // Persistable JSON array of collapsed folder ids (numeric).
    [[nodiscard]] std::string collapse_json() const;
    [[nodiscard]] int scroll_offset() const;
    // Painted list length including the leading "+ New" row.
    [[nodiscard]] int list_row_count() const;

    // Valid only immediately after handle_key() returns RenameCommit —
    // returns the edited text and clears the internal buffer. Also clears
    // creating_folder / rename-target state; read those *before* calling.
    [[nodiscard]] std::string take_rename_buffer();
    // True after RenameCommit when the edit was "new folder" name entry
    // (vs renaming an existing chat/folder). Cleared by take_rename_buffer().
    [[nodiscard]] bool is_creating_folder() const;
    // Target id captured when rename began (conversation or folder). Empty
    // when creating a folder. Cleared by take_rename_buffer().
    [[nodiscard]] std::string rename_target_id() const;
    [[nodiscard]] bool rename_target_is_folder() const;
    // Valid only after MoveCommit — empty string means unfiled.
    [[nodiscard]] std::string take_move_folder_id();

    HistorySidebarKey handle_key(int key_byte, char csi_final = 0, const std::string& csi_params = {});
    [[nodiscard]] HistorySidebarSnapshot snapshot() const;

private:
    enum class Mode { Normal, Renaming, ConfirmDelete, Menu, Moving };
    enum class PinKind { New, Folder, Conversation };

    // Conversation menu: Open / Rename / Move to… / Delete.
    static constexpr int kConvMenuOpen = 0;
    static constexpr int kConvMenuRename = 1;
    static constexpr int kConvMenuMove = 2;
    static constexpr int kConvMenuDelete = 3;
    static constexpr int kConvMenuCount = 4;
    // Folder menu: Rename / New chat here / Delete folder.
    static constexpr int kFoldMenuRename = 0;
    static constexpr int kFoldMenuNew = 1;
    static constexpr int kFoldMenuDelete = 2;
    static constexpr int kFoldMenuCount = 3;
    // "+ New" row menu: New folder.
    static constexpr int kNewMenuFolder = 0;
    static constexpr int kNewMenuCount = 1;

    // Assumes mu_ is already held by the caller.
    int index_for_pin_locked() const;
    void set_pin_from_index_locked(int idx);
    // Keep pin_kind_/pin_id_ on a currently painted row (folder gone,
    // conversation soft-deleted, or chat hidden under a collapsed folder).
    void ensure_pin_visible_locked();
    // `visible_lines` is the terminal line budget from
    // history_sidebar_visible_rows (not a raw list-row count).
    void clamp_scroll_locked(int idx, int visible_lines);
    std::string current_title_locked() const;
    std::vector<HistorySidebarRow> build_rows_locked() const;
    void load_collapse_locked(const ConversationStore& store);
    void toggle_folder_locked(const std::string& folder_id);
    HistorySidebarKey commit_menu_locked();
    void begin_move_locked();
    HistorySidebarKey begin_new_folder_locked();
    HistorySidebarKey begin_rename_locked();

    mutable std::mutex mu_;
    bool enabled_ = true;
    bool focused_ = false;
    // Selection pinned by kind + id so background re-sorts / collapse
    // toggles never silently move the cursor to a different row.
    PinKind pin_kind_ = PinKind::New;
    std::string pin_id_;
    int  scroll_offset_ = 0;
    // Last line budget from clamp_scroll / navigation; used by refresh_entries
    // to keep the pinned row on-screen after the pin jumps.
    int  last_visible_lines_ = 0;
    std::string active_id_;
    std::vector<ConversationEntry> entries_;
    std::vector<ConversationFolderEntry> folders_;
    std::unordered_set<std::string> collapsed_;

    Mode mode_ = Mode::Normal;
    std::string rename_buffer_;
    bool creating_folder_ = false;
    // Pinned when rename begins so RenameCommit still knows the target
    // after mode returns to Normal (pin can move under a concurrent refresh).
    std::string rename_target_id_;
    bool rename_target_is_folder_ = false;
    int menu_index_ = 0;
    // Move picker: parallel label + folder_id lists ("" = unfiled).
    int move_index_ = 0;
    std::vector<std::string> move_labels_;
    std::vector<std::string> move_folder_ids_;
    std::string move_result_;
    std::string move_from_folder_id_;
};

// Read one key for sidebar navigation (arrows, enter, esc, PgUp/PgDn).
// Returns key byte; sets csi_final for CSI sequences (A/B/~) and csi_params
// to the numeric parameter string (e.g. "5" for PgUp's ESC[5~).
int read_history_sidebar_key(char& csi_final, std::string& csi_params);

} // namespace arbiter
