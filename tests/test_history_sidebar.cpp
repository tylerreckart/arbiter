#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "repl/conversation_store.h"
#include "tui/history_sidebar.h"

#include <filesystem>
#include <random>
#include <sstream>
#include <thread>

using namespace arbiter;
namespace fs = std::filesystem;

namespace {

std::string make_temp_dir() {
    static std::atomic<int> counter{0};
    std::random_device rd;
    std::ostringstream name;
    name << "arbiter_history_sidebar_test_" << rd() << "_" << counter++;
    const fs::path dir = fs::temp_directory_path() / name.str();
    fs::create_directories(dir);
    return dir.string();
}

} // namespace

TEST_CASE("selection stays pinned to its conversation across a background reorder") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string first = store.active_id();
    const std::string second = store.create(dir);
    const std::string third = store.create(dir);
    store.set_active(first); // just changes the active pointer, not order

    HistorySidebarState sidebar;
    sidebar.set_enabled(true, dir);
    sidebar.enter_focus(store, first);

    // Entries sorted most-recently-updated first: third, second, first.
    // Move down twice to land on `first` (row 3).
    sidebar.move_selection(1, 10);
    sidebar.move_selection(1, 10);
    REQUIRE(sidebar.selected_conversation_id() == first);

    // Reorder the list out from under the selection: bump `second`'s
    // updated_at so it moves ahead of `first` without touching selection.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    store.set_title(second, "bumped");
    sidebar.refresh_entries(store);

    CHECK(sidebar.selected_conversation_id() == first);

    fs::remove_all(dir);
}

TEST_CASE("j/k are vim-style aliases for Down/Up") {
    HistorySidebarState sidebar;
    CHECK(sidebar.handle_key('j', 0, "") == HistorySidebarKey::Down);
    CHECK(sidebar.handle_key('k', 0, "") == HistorySidebarKey::Up);
}

TEST_CASE("n starts a new conversation regardless of selection") {
    HistorySidebarState sidebar;
    CHECK(sidebar.handle_key('n', 0, "") == HistorySidebarKey::New);
}

TEST_CASE("PgUp/PgDn map from their CSI sequences") {
    HistorySidebarState sidebar;
    CHECK(sidebar.handle_key(0x1B, '~', "5") == HistorySidebarKey::PageUp);
    CHECK(sidebar.handle_key(0x1B, '~', "6") == HistorySidebarKey::PageDown);
}

TEST_CASE("rename: r enters edit mode pre-filled with the title, Enter commits") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    store.set_title(store.active_id(), "original title");

    HistorySidebarState sidebar;
    sidebar.enter_focus(store, store.active_id());

    CHECK(sidebar.handle_key('r', 0, "") == HistorySidebarKey::RenameStart);
    CHECK(sidebar.snapshot().renaming);
    CHECK(sidebar.snapshot().rename_buffer == "original title");

    // Backspace three times, type "!!!"
    CHECK(sidebar.handle_key(127, 0, "") == HistorySidebarKey::None);
    CHECK(sidebar.handle_key(127, 0, "") == HistorySidebarKey::None);
    CHECK(sidebar.handle_key(127, 0, "") == HistorySidebarKey::None);
    sidebar.handle_key('!', 0, "");
    sidebar.handle_key('!', 0, "");
    sidebar.handle_key('!', 0, "");

    CHECK(sidebar.handle_key('\r', 0, "") == HistorySidebarKey::RenameCommit);
    CHECK(sidebar.take_rename_buffer() == "original ti!!!");
    CHECK_FALSE(sidebar.snapshot().renaming);

    fs::remove_all(dir);
}

TEST_CASE("rename: kitty CSI-u Enter commits (not Esc-cancel)") {
    // OpenTUI's kitty handshake encodes Enter/Esc/Backspace as CSI-u.
    // Before decode, Enter arrived as Esc+'u' and aborted rename.
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    store.set_title(store.active_id(), "before");

    HistorySidebarState sidebar;
    sidebar.enter_focus(store, store.active_id());
    REQUIRE(sidebar.handle_key('r', 0, "") == HistorySidebarKey::RenameStart);

    while (!sidebar.snapshot().rename_buffer.empty()) {
        // Kitty Backspace: CSI 127 u
        CHECK(sidebar.handle_key(0x1B, 'u', "127") == HistorySidebarKey::None);
    }
    for (char c : std::string("after")) sidebar.handle_key(c, 0, "");

    CHECK(sidebar.handle_key(0x1B, 'u', "13") == HistorySidebarKey::RenameCommit);
    CHECK(sidebar.take_rename_buffer() == "after");
    CHECK_FALSE(sidebar.snapshot().renaming);

    // Esc via CSI-u cancels rename but stays focused.
    sidebar.handle_key('r', 0, "");
    sidebar.handle_key('x', 0, "");
    CHECK(sidebar.handle_key(0x1B, 'u', "27") == HistorySidebarKey::None);
    CHECK(sidebar.take_rename_buffer().empty());
    CHECK_FALSE(sidebar.snapshot().renaming);
    CHECK(sidebar.focused());

    fs::remove_all(dir);
}

TEST_CASE("rename: kitty CSI-u printable with text field inserts") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    store.set_title(store.active_id(), "x");

    HistorySidebarState sidebar;
    sidebar.enter_focus(store, store.active_id());
    REQUIRE(sidebar.handle_key('r') == HistorySidebarKey::RenameStart);
    while (!sidebar.snapshot().rename_buffer.empty()) {
        sidebar.handle_key(0x1B, 'u', "127;1:1;127");
    }

    CHECK(sidebar.handle_key(0x1B, 'u', "97;1:1;97") == HistorySidebarKey::None);
    CHECK(sidebar.handle_key(0x1B, 'u', "98;1;98") == HistorySidebarKey::None);
    CHECK(sidebar.snapshot().rename_buffer == "ab");
    // Spurious cursor-position CSI must not abort rename.
    CHECK(sidebar.handle_key(0x1B, 'R', "1;1") == HistorySidebarKey::None);
    CHECK(sidebar.snapshot().renaming);
    CHECK(sidebar.snapshot().rename_buffer == "ab");

    fs::remove_all(dir);
}

TEST_CASE("rename: empty title is rejected; caption distinguishes folder") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    const std::string fid = store.create_folder("Labs");

    HistorySidebarState sidebar;
    sidebar.enter_focus(store, store.active_id());
    // Row 0 = + New, row 1 = first folder.
    sidebar.select_at_index(1, 20);
    REQUIRE(sidebar.is_folder_selected());
    CHECK(sidebar.selected_folder_id() == fid);

    CHECK(sidebar.handle_key('r') == HistorySidebarKey::RenameStart);
    auto snap = sidebar.snapshot();
    CHECK(snap.renaming);
    CHECK(snap.rename_is_folder);
    CHECK(snap.rename_buffer == "Labs");

    // Wipe the name; Enter must stay in rename mode.
    while (!sidebar.snapshot().rename_buffer.empty()) {
        CHECK(sidebar.handle_key(127) == HistorySidebarKey::None);
    }
    CHECK(sidebar.handle_key('\r') == HistorySidebarKey::None);
    CHECK(sidebar.snapshot().renaming);

    sidebar.handle_key('X');
    CHECK(sidebar.handle_key('\r') == HistorySidebarKey::RenameCommit);
    CHECK(sidebar.take_rename_buffer() == "X");

    fs::remove_all(dir);
}

TEST_CASE("rename: conversation commit keeps stashed target id") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    const std::string id = store.active_id();
    store.set_title(id, "original");

    HistorySidebarState sidebar;
    sidebar.set_enabled(true, dir);
    sidebar.enter_focus(store, id);
    REQUIRE(sidebar.handle_key('r') == HistorySidebarKey::RenameStart);
    CHECK(sidebar.rename_target_id() == id);
    CHECK_FALSE(sidebar.rename_target_is_folder());
    CHECK_FALSE(sidebar.is_creating_folder());

    while (!sidebar.snapshot().rename_buffer.empty()) {
        sidebar.handle_key(127);
    }
    for (char c : std::string("renamed")) sidebar.handle_key(c);
    CHECK(sidebar.handle_key('\r') == HistorySidebarKey::RenameCommit);

    // Target must still be readable before take_rename_buffer clears it.
    CHECK(sidebar.rename_target_id() == id);
    CHECK_FALSE(sidebar.rename_target_is_folder());
    CHECK(sidebar.take_rename_buffer() == "renamed");
    CHECK(sidebar.rename_target_id().empty());

    store.set_title_locked(id, "renamed");
    sidebar.refresh_entries(store);
    bool found = false;
    for (const auto& e : sidebar.snapshot().entries) {
        if (e.id == id) {
            CHECK(e.title == "renamed");
            found = true;
        }
    }
    CHECK(found);

    fs::remove_all(dir);
}

TEST_CASE("rename: Esc cancels without surfacing a commit") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    HistorySidebarState sidebar;
    sidebar.enter_focus(store, store.active_id());

    sidebar.handle_key('r', 0, "");
    sidebar.handle_key('x', 0, "");
    // Esc cancels rename but keeps sidebar focus (second Esc exits).
    CHECK(sidebar.handle_key(0x1B, 0, "") == HistorySidebarKey::None);
    CHECK_FALSE(sidebar.snapshot().renaming);
    CHECK(sidebar.focused());
    CHECK(sidebar.take_rename_buffer().empty());
    CHECK(sidebar.handle_key(0x1B, 0, "") == HistorySidebarKey::Escape);

    fs::remove_all(dir);
}

TEST_CASE("delete: d then y confirms, d then anything else cancels") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    HistorySidebarState sidebar;
    sidebar.enter_focus(store, store.active_id());

    CHECK(sidebar.handle_key('d', 0, "") == HistorySidebarKey::DeleteStart);
    CHECK(sidebar.snapshot().confirming_delete);
    CHECK(sidebar.handle_key('y', 0, "") == HistorySidebarKey::DeleteConfirmed);
    CHECK_FALSE(sidebar.snapshot().confirming_delete);

    sidebar.handle_key('d', 0, "");
    CHECK(sidebar.handle_key('q', 0, "") == HistorySidebarKey::None);
    CHECK_FALSE(sidebar.snapshot().confirming_delete);

    fs::remove_all(dir);
}

TEST_CASE("rename/delete are no-ops on the '+ New conversation' row; m opens New folder") {
    HistorySidebarState sidebar; // pinned_new_ defaults true, no store needed
    CHECK(sidebar.handle_key('r', 0, "") == HistorySidebarKey::None);
    CHECK_FALSE(sidebar.snapshot().renaming);
    CHECK(sidebar.handle_key('d', 0, "") == HistorySidebarKey::None);
    CHECK_FALSE(sidebar.snapshot().confirming_delete);
    CHECK(sidebar.handle_key('m', 0, "") == HistorySidebarKey::MenuOpen);
    CHECK(sidebar.snapshot().menu_open);
    CHECK(sidebar.snapshot().menu_is_new);
    CHECK(sidebar.handle_key('f', 0, "") == HistorySidebarKey::RenameStart);
    CHECK(sidebar.snapshot().creating_folder);
    CHECK(sidebar.is_creating_folder());
}

TEST_CASE("menu: m opens Open/Rename/Delete; Enter commits; Esc cancels") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    HistorySidebarState sidebar;
    sidebar.enter_focus(store, store.active_id());

    CHECK(sidebar.handle_key('m', 0, "") == HistorySidebarKey::MenuOpen);
    auto snap = sidebar.snapshot();
    CHECK(snap.menu_open);
    CHECK(snap.menu_index == 0);

    // Down moves highlight to Rename; Esc cancels without acting.
    CHECK(sidebar.handle_key('j', 0, "") == HistorySidebarKey::None);
    CHECK(sidebar.snapshot().menu_index == 1);
    CHECK(sidebar.handle_key(0x1B, 0, "") == HistorySidebarKey::None);
    CHECK_FALSE(sidebar.snapshot().menu_open);

    // Re-open and commit Open (default highlight).
    CHECK(sidebar.handle_key('m', 0, "") == HistorySidebarKey::MenuOpen);
    CHECK(sidebar.handle_key('\r', 0, "") == HistorySidebarKey::Enter);
    CHECK_FALSE(sidebar.snapshot().menu_open);

    // First-letter shortcut: r jumps to rename.
    CHECK(sidebar.handle_key('m', 0, "") == HistorySidebarKey::MenuOpen);
    CHECK(sidebar.handle_key('r', 0, "") == HistorySidebarKey::RenameStart);
    CHECK(sidebar.snapshot().renaming);
    CHECK_FALSE(sidebar.snapshot().menu_open);
    sidebar.handle_key(0x1B, 0, "");   // cancel rename

    // Menu → Delete lands in the same confirm step as a bare 'd'.
    CHECK(sidebar.handle_key('m', 0, "") == HistorySidebarKey::MenuOpen);
    CHECK(sidebar.handle_key('d', 0, "") == HistorySidebarKey::DeleteStart);
    CHECK(sidebar.snapshot().confirming_delete);
    CHECK_FALSE(sidebar.snapshot().menu_open);
    CHECK(sidebar.handle_key('y', 0, "") == HistorySidebarKey::DeleteConfirmed);

    fs::remove_all(dir);
}

TEST_CASE("'f' starts new-folder name entry; Enter commits") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    HistorySidebarState sidebar;
    sidebar.set_enabled(true, dir);
    sidebar.enter_focus(store, store.active_id());

    CHECK(sidebar.handle_key('f') == HistorySidebarKey::RenameStart);
    auto snap = sidebar.snapshot();
    CHECK(snap.renaming);
    CHECK(snap.creating_folder);
    CHECK(snap.rename_buffer.empty());
    CHECK_FALSE(snap.rename_is_folder);

    for (char c : std::string("Labs")) sidebar.handle_key(c);
    CHECK(sidebar.snapshot().rename_buffer == "Labs");
    CHECK(sidebar.is_creating_folder());
    CHECK(sidebar.handle_key('\r') == HistorySidebarKey::RenameCommit);
    CHECK(sidebar.is_creating_folder());
    CHECK(sidebar.take_rename_buffer() == "Labs");
    CHECK_FALSE(sidebar.is_creating_folder());

    fs::remove_all(dir);
}

TEST_CASE("new folder: empty name rejected; Esc cancels") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    HistorySidebarState sidebar;
    sidebar.set_enabled(true, dir);
    sidebar.enter_focus(store, store.active_id());

    sidebar.handle_key('f');
    CHECK(sidebar.handle_key('\r') == HistorySidebarKey::None);
    CHECK(sidebar.snapshot().creating_folder);

    sidebar.handle_key('x');
    CHECK(sidebar.handle_key(0x1B) == HistorySidebarKey::None);
    CHECK_FALSE(sidebar.snapshot().renaming);
    CHECK_FALSE(sidebar.is_creating_folder());
    CHECK(sidebar.focused());
    CHECK(sidebar.handle_key(0x1B) == HistorySidebarKey::Escape);

    fs::remove_all(dir);
}

TEST_CASE("folder tree: headers, collapse, and new-in-folder") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string unfiled = store.active_id();
    store.set_title(unfiled, "loose");
    const std::string fid = store.create_folder("Work");
    REQUIRE_FALSE(fid.empty());
    const std::string child = store.create(dir, fid);
    store.set_title(child, "nested");

    HistorySidebarState sidebar;
    sidebar.set_enabled(true, dir);
    sidebar.enter_focus(store, child);

    auto snap = sidebar.snapshot();
    // + New, Folders, Work, nested, Chats, loose.
    REQUIRE(snap.rows.size() == 6);
    CHECK(snap.rows[0].kind == HistorySidebarRowKind::New);
    CHECK(snap.rows[1].kind == HistorySidebarRowKind::Section);
    CHECK(snap.rows[1].title == "Folders");
    CHECK(snap.rows[2].kind == HistorySidebarRowKind::Folder);
    CHECK(snap.rows[2].id == fid);
    CHECK(snap.rows[2].expanded);
    CHECK(snap.rows[3].kind == HistorySidebarRowKind::Conversation);
    CHECK(snap.rows[3].id == child);
    CHECK(snap.rows[3].indent == 1);
    CHECK(snap.rows[4].kind == HistorySidebarRowKind::Section);
    CHECK(snap.rows[4].title == "Chats");
    CHECK(snap.rows[5].kind == HistorySidebarRowKind::Conversation);
    CHECK(snap.rows[5].id == unfiled);
    CHECK(snap.rows[5].indent == 0);

    // Pin the folder header (skip the Folders section) and collapse it.
    sidebar.select_at_index(2, 10);
    CHECK(sidebar.is_folder_selected());
    CHECK(sidebar.selected_folder_id() == fid);
    CHECK(sidebar.handle_key('\r') == HistorySidebarKey::ToggleFolder);
    store.set_folder_collapse_json(sidebar.collapse_json());

    snap = sidebar.snapshot();
    // Collapsed: + New, Folders, Work [+], Chats, loose — nested hidden.
    REQUIRE(snap.rows.size() == 5);
    CHECK(snap.rows[2].kind == HistorySidebarRowKind::Folder);
    CHECK_FALSE(snap.rows[2].expanded);
    CHECK(snap.rows[4].id == unfiled);

    // New while folder is selected files into that folder.
    CHECK(sidebar.new_target_folder_id() == fid);
    CHECK(sidebar.handle_key('n') == HistorySidebarKey::New);

    // Expand again and select the child — new still targets Work.
    sidebar.handle_key('\r');  // toggle expand
    sidebar.select_at_index(3, 10);
    REQUIRE(sidebar.selected_conversation_id() == child);
    CHECK(sidebar.new_target_folder_id() == fid);

    // Unfiled selection → empty folder target.
    sidebar.select_at_index(5, 10);
    REQUIRE(sidebar.selected_conversation_id() == unfiled);
    CHECK(sidebar.new_target_folder_id().empty());

    // Section rows are skipped when moving selection.
    sidebar.select_at_index(0, 10);  // + New
    sidebar.move_selection(1, 10);   // should land on Work, not "Folders"
    CHECK(sidebar.is_folder_selected());
    CHECK(sidebar.selected_folder_id() == fid);

    fs::remove_all(dir);
}

TEST_CASE("conversation menu Move to… opens picker and commits") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string cid = store.active_id();
    const std::string fid = store.create_folder("Archive");

    HistorySidebarState sidebar;
    sidebar.set_enabled(true, dir);
    sidebar.enter_focus(store, cid);

    CHECK(sidebar.handle_key('m') == HistorySidebarKey::MenuOpen);
    // Open / Rename / Move to… / Delete — jump to Move via 'v'.
    CHECK(sidebar.handle_key('v') == HistorySidebarKey::MoveStart);
    CHECK(sidebar.snapshot().moving);
    REQUIRE(sidebar.snapshot().move_labels.size() >= 2);

    // Select Archive (first label) and commit.
    sidebar.handle_key('k');  // may already be on Archive; ensure wrap/nav works
    // Find Archive index by committing from index 0 after resetting via Esc+reopen.
    sidebar.handle_key(0x1B);  // cancel move
    CHECK_FALSE(sidebar.snapshot().moving);

    sidebar.handle_key('m');
    sidebar.handle_key('v');
    // move_index defaults to current folder (unfiled = last).
    // Move up to Archive if needed.
    while (sidebar.snapshot().move_index
           != static_cast<int>(sidebar.snapshot().move_labels.size()) - 2
           && sidebar.snapshot().moving) {
        sidebar.handle_key('k');
    }
    CHECK(sidebar.handle_key('\r') == HistorySidebarKey::MoveCommit);
    const std::string target = sidebar.take_move_folder_id();
    CHECK(target == fid);
    CHECK(store.move_to_folder(cid, target));

    auto entries = store.list();
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const ConversationEntry& e) { return e.id == cid; });
    REQUIRE(it != entries.end());
    CHECK(it->folder_id == fid);

    fs::remove_all(dir);
}
