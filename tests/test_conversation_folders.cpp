#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "repl/conversation_store.h"
#include "tenant_store.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <random>
#include <sstream>

using namespace arbiter;
namespace fs = std::filesystem;

namespace {

std::string make_temp_dir() {
    static std::atomic<int> counter{0};
    std::random_device rd;
    std::ostringstream name;
    name << "arbiter_folders_test_" << rd() << "_" << counter++;
    const fs::path dir = fs::temp_directory_path() / name.str();
    fs::create_directories(dir);
    return dir.string();
}

} // namespace

TEST_CASE("conversation folder CRUD and membership") {
    const std::string dir = make_temp_dir();
    TenantStore s;
    s.open(dir + "/tenants.db");
    const int64_t tid = s.create_tenant("acme").tenant.id;

    auto folder = s.create_conversation_folder(tid, "Research");
    CHECK(folder.id > 0);
    CHECK(folder.name == "Research");
    CHECK(folder.position == 0);

    auto listed = s.list_conversation_folders(tid);
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].id == folder.id);

    auto api = s.create_conversation(tid, "API thread", "index");
    CHECK(api.folder_id == 0);
    CHECK(s.set_conversation_folder(tid, api.id, folder.id));
    auto got = s.get_conversation(tid, api.id);
    REQUIRE(got);
    CHECK(got->folder_id == folder.id);

    auto scoped = s.list_conversations(tid, 0, 50, folder.id);
    REQUIRE(scoped.size() == 1);
    CHECK(scoped[0].id == api.id);

    auto unfiled = s.list_conversations(tid, 0, 50, /*folder_id_filter=*/0);
    CHECK(unfiled.empty());

    CHECK(s.update_conversation_folder(tid, folder.id, "Labs", /*pos=*/-1));
    CHECK(s.get_conversation_folder(tid, folder.id)->name == "Labs");

    CHECK(s.delete_conversation_folder(tid, folder.id));
    CHECK_FALSE(s.get_conversation_folder(tid, folder.id).has_value());
    got = s.get_conversation(tid, api.id);
    REQUIRE(got);
    CHECK(got->folder_id == 0);

    fs::remove_all(dir);
}

TEST_CASE("ConversationStore folder create/move/new-in-folder") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string fid = store.create_folder("Work");
    REQUIRE_FALSE(fid.empty());

    const std::string cid = store.create(fs::current_path().string(), fid);
    auto entries = store.list();
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const ConversationEntry& e) { return e.id == cid; });
    REQUIRE(it != entries.end());
    CHECK(it->folder_id == fid);

    const std::string other = store.create(fs::current_path().string());
    CHECK(store.move_to_folder(other, fid));
    entries = store.list();
    it = std::find_if(entries.begin(), entries.end(),
                      [&](const ConversationEntry& e) { return e.id == other; });
    REQUIRE(it != entries.end());
    CHECK(it->folder_id == fid);

    CHECK(store.move_to_folder(other, /*folder_id=*/""));
    entries = store.list();
    it = std::find_if(entries.begin(), entries.end(),
                      [&](const ConversationEntry& e) { return e.id == other; });
    REQUIRE(it != entries.end());
    CHECK(it->folder_id.empty());

    CHECK(store.delete_folder(fid));
    entries = store.list();
    it = std::find_if(entries.begin(), entries.end(),
                      [&](const ConversationEntry& e) { return e.id == cid; });
    REQUIRE(it != entries.end());
    CHECK(it->folder_id.empty());
    CHECK(store.list_folders().empty());

    fs::remove_all(dir);
}
