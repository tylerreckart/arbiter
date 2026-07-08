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

TEST_CASE("rename: Esc cancels without surfacing a commit") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    HistorySidebarState sidebar;
    sidebar.enter_focus(store, store.active_id());

    sidebar.handle_key('r', 0, "");
    sidebar.handle_key('x', 0, "");
    CHECK(sidebar.handle_key(0x1B, 0, "") == HistorySidebarKey::Escape);
    CHECK_FALSE(sidebar.snapshot().renaming);
    CHECK(sidebar.take_rename_buffer().empty());

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

TEST_CASE("rename/delete are no-ops on the '+ New conversation' row") {
    HistorySidebarState sidebar; // pinned_new_ defaults true, no store needed
    CHECK(sidebar.handle_key('r', 0, "") == HistorySidebarKey::None);
    CHECK_FALSE(sidebar.snapshot().renaming);
    CHECK(sidebar.handle_key('d', 0, "") == HistorySidebarKey::None);
    CHECK_FALSE(sidebar.snapshot().confirming_delete);
}

TEST_CASE("'/' filters entries; Enter commits; Esc clears") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string alpha = store.active_id();
    const std::string beta = store.create(dir);
    const std::string gamma = store.create(dir);
    store.set_title(alpha, "alpha task");
    store.set_title(beta, "beta task");
    store.set_title(gamma, "gamma other");

    HistorySidebarState sidebar;
    sidebar.set_enabled(true, dir);
    sidebar.enter_focus(store, gamma);

    CHECK(sidebar.handle_key('/') == HistorySidebarKey::None);
    CHECK(sidebar.snapshot().filtering);

    sidebar.handle_key('t');
    sidebar.handle_key('a');
    sidebar.handle_key('s');
    sidebar.handle_key('k');
    auto snap = sidebar.snapshot();
    CHECK(snap.filter == "task");
    REQUIRE(snap.entries.size() == 2);
    // gamma was pinned but "task" filters it out — pin moves to the first
    // visible entry.
    const std::string selected = sidebar.selected_conversation_id();
    CHECK((selected == alpha || selected == beta));
    CHECK(selected != gamma);

    // Enter commits: filter stays applied, edit mode ends.
    CHECK(sidebar.handle_key('\r') == HistorySidebarKey::None);
    snap = sidebar.snapshot();
    CHECK_FALSE(snap.filtering);
    CHECK(snap.filter == "task");
    CHECK(snap.entries.size() == 2);

    // First Esc clears the applied filter instead of closing the sidebar;
    // the second Esc surfaces Escape as usual.
    CHECK(sidebar.handle_key(0x1B) == HistorySidebarKey::None);
    snap = sidebar.snapshot();
    CHECK(snap.filter.empty());
    CHECK(snap.entries.size() == 3);
    CHECK(sidebar.handle_key(0x1B) == HistorySidebarKey::Escape);

    fs::remove_all(dir);
}

TEST_CASE("filter edit: backspace re-widens; Esc while typing cancels") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string alpha = store.active_id();
    const std::string beta = store.create(dir);
    store.set_title(alpha, "deploy pipeline");
    store.set_title(beta, "deploy docs");

    HistorySidebarState sidebar;
    sidebar.set_enabled(true, dir);
    sidebar.enter_focus(store, alpha);

    sidebar.handle_key('/');
    for (char c : std::string("deploy p")) sidebar.handle_key(c);
    CHECK(sidebar.snapshot().entries.size() == 1);

    sidebar.handle_key(127);   // backspace: "deploy " matches both again
    sidebar.handle_key(127);
    CHECK(sidebar.snapshot().entries.size() == 2);

    // Esc while editing cancels filtering entirely.
    CHECK(sidebar.handle_key(0x1B) == HistorySidebarKey::None);
    auto snap = sidebar.snapshot();
    CHECK_FALSE(snap.filtering);
    CHECK(snap.filter.empty());
    CHECK(snap.entries.size() == 2);

    fs::remove_all(dir);
}

TEST_CASE("arrows navigate the filtered list while typing") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string alpha = store.active_id();
    const std::string beta = store.create(dir);
    (void)store.create(dir);   // unrelated third entry
    store.set_title(alpha, "fix login bug");
    store.set_title(beta, "fix logout bug");

    HistorySidebarState sidebar;
    sidebar.set_enabled(true, dir);
    sidebar.enter_focus(store, alpha);

    sidebar.handle_key('/');
    for (char c : std::string("fix")) sidebar.handle_key(c);
    REQUIRE(sidebar.snapshot().entries.size() == 2);

    // Arrow keys surface Up/Down while filtering.
    CHECK(sidebar.handle_key(0x1B, 'B') == HistorySidebarKey::Down);
    CHECK(sidebar.handle_key(0x1B, 'A') == HistorySidebarKey::Up);

    // enter_focus pinned the active conversation (alpha), which sorts last
    // of the two matches — walk up to the other match, then back down.
    REQUIRE(sidebar.selected_conversation_id() == alpha);
    sidebar.move_selection(-1, 10);
    CHECK(sidebar.selected_conversation_id() == beta);
    sidebar.move_selection(1, 10);
    CHECK(sidebar.selected_conversation_id() == alpha);
    // A further Down clamps at the last filtered row: the third (unfiltered)
    // conversation is never reachable while the filter is applied.
    sidebar.move_selection(1, 10);
    CHECK(sidebar.selected_conversation_id() == alpha);

    fs::remove_all(dir);
}
