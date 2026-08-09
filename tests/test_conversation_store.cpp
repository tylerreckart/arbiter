#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "atomic_file.h"
#include "repl/conversation_store.h"
#include "tenant_store.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <sstream>
#include <thread>
#include <unistd.h>

using namespace arbiter;
namespace fs = std::filesystem;

namespace {

std::string make_temp_dir() {
    static std::atomic<int> counter{0};
    std::random_device rd;
    std::ostringstream name;
    name << "arbiter_convstore_test_" << rd() << "_" << counter++;
    const fs::path dir = fs::temp_directory_path() / name.str();
    fs::create_directories(dir);
    return dir.string();
}

std::string read_all(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int64_t to_db_id(const std::string& id) {
    return static_cast<int64_t>(std::stoll(id));
}

void write_session(ConversationStore& store, const std::string& id,
                   const std::string& body) {
    store.tenant_store().set_conversation_session_json(
        store.tenant_id(), to_db_id(id), body, /*bump_updated_at=*/false);
}

} // namespace

TEST_CASE("create-on-empty-store does not deadlock") {
    const std::string dir = make_temp_dir();

    std::promise<std::string> done;
    std::future<std::string> fut = done.get_future();
    std::thread t([&]() {
        ConversationStore store(dir);
        done.set_value(store.active_id());
    });

    const auto status = fut.wait_for(std::chrono::seconds(5));
    CHECK(status == std::future_status::ready);
    if (status == std::future_status::ready) {
        CHECK_FALSE(fut.get().empty());
        t.join();
    } else {
        t.detach();
    }

    fs::remove_all(dir);
}

TEST_CASE("atomic_write_file leaves no partial file and no stray tmp file") {
    const std::string dir = make_temp_dir();
    const std::string path = dir + "/data.json";

    CHECK(atomic_write_file(path, "hello world"));
    CHECK(fs::exists(path));
    CHECK_FALSE(fs::exists(path + ".tmp"));
    CHECK(read_all(path) == "hello world");

    CHECK(atomic_write_file(path, "second write"));
    CHECK(read_all(path) == "second write");
    CHECK_FALSE(fs::exists(path + ".tmp"));

    fs::remove_all(dir);
}

TEST_CASE("legacy JSON store migrates into sqlite on first open") {
    const std::string dir = make_temp_dir();
    const std::string conv_dir = dir + "/conversations";
    fs::create_directories(conv_dir);

    {
        std::ofstream f(conv_dir + "/manifest.json");
        f << "{ this is not valid json";
    }
    {
        std::ofstream f(conv_dir + "/deadbeefcafebabe.json");
        f << R"({"version":1,"index":[{"role":"user","content":"hi"}],"agents":{}})";
    }

    ConversationStore store(dir);
    const auto entries = store.list();

    bool found = false;
    for (const auto& e : entries) {
        if (e.title == "Untitled (recovered)") {
            found = true;
            CHECK_FALSE(store.session_json(e.id).empty());
            CHECK(store.session_json(e.id).find("hi") != std::string::npos);
        }
    }
    CHECK(found);
    CHECK(store.tenant_store().tui_conversations_migrated(store.tenant_id()));

    fs::remove_all(dir);
}

TEST_CASE("soft delete filters list() but keeps session_json; purge removes it") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string a = store.create(dir);
    const std::string b = store.create(dir);

    store.soft_delete(a);
    {
        const auto entries = store.list();
        bool a_visible = false;
        for (const auto& e : entries) if (e.id == a) a_visible = true;
        CHECK_FALSE(a_visible);
    }
    CHECK_FALSE(store.session_json(a).empty());

    store.purge(b);
    CHECK(store.session_json(b).empty());

    fs::remove_all(dir);
}

TEST_CASE("soft-deleting the active conversation reassigns active to another entry") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string first_active = store.active_id();
    const std::string second = store.create(dir);
    CHECK(store.active_id() == second);

    store.soft_delete(second);
    CHECK(store.active_id() == first_active);

    fs::remove_all(dir);
}

TEST_CASE("soft-deleting the only conversation creates a fresh one") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string only = store.active_id();
    store.soft_delete(only);

    CHECK(store.active_id() != only);
    const auto entries = store.list();
    CHECK(entries.size() == 1);
    CHECK(entries.front().id == store.active_id());

    fs::remove_all(dir);
}

TEST_CASE("create_or_reuse reuses an empty active conversation instead of creating another") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string before = store.active_id();
    const std::string after = store.create_or_reuse(dir);
    CHECK(after == before);
    CHECK(store.list().size() == 1);

    fs::remove_all(dir);
}

TEST_CASE("create_or_reuse_for falls back to empty active when prefer_id is empty") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string before = store.active_id();
    const std::string after = store.create_or_reuse_for(dir, /*prefer_id=*/"");
    CHECK(after == before);
    CHECK(store.list().size() == 1);

    fs::remove_all(dir);
}

TEST_CASE("create_or_reuse_for does not steal empty active when prefer has turns") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string empty_active = store.active_id();
    const std::string busy = store.create(dir);
    write_session(store, busy, R"({"index":[{"role":"user","content":"hi"}]})");
    store.set_active(empty_active);

    const std::string after = store.create_or_reuse_for(dir, busy);
    CHECK(after != empty_active);
    CHECK(after != busy);
    CHECK(store.list().size() == 3);

    fs::remove_all(dir);
}

TEST_CASE("create_or_reuse_for with empty folder_id unfiles a reused chat") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    const std::string fid = store.create_folder("Work");
    const std::string id = store.create(dir, fid);
    REQUIRE(store.list().front().folder_id == fid);

    const std::string after = store.create_or_reuse_for(dir, id, /*folder_id=*/"");
    CHECK(after == id);
    CHECK(store.list().front().folder_id.empty());

    fs::remove_all(dir);
}

TEST_CASE("create with a deleted folder id files as unfiled instead of throwing") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    const std::string fid = store.create_folder("Temp");
    REQUIRE(store.delete_folder(fid));
    std::string id;
    CHECK_NOTHROW(id = store.create(dir, fid));
    CHECK_FALSE(id.empty());
    CHECK(store.list().front().folder_id.empty());

    fs::remove_all(dir);
}

TEST_CASE("set_title does not lock; set_title_locked and lock_title do") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    const std::string id = store.active_id();

    CHECK_FALSE(store.is_titled(id));

    store.set_title(id, "deterministic title");
    CHECK(store.list().front().title == "deterministic title");
    CHECK_FALSE(store.is_titled(id));

    store.set_title(id, "deterministic title v2");
    CHECK(store.list().front().title == "deterministic title v2");
    CHECK_FALSE(store.is_titled(id));

    store.set_title_locked(id, "model refined title");
    CHECK(store.list().front().title == "model refined title");
    CHECK(store.is_titled(id));

    store.set_title(id, "should not apply");
    CHECK(store.list().front().title == "model refined title");
    CHECK(store.is_titled(id));

    fs::remove_all(dir);
}

TEST_CASE("lock_title locks without changing the title text") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    const std::string id = store.active_id();

    store.set_title(id, "deterministic title");
    store.lock_title(id);
    CHECK(store.list().front().title == "deterministic title");
    CHECK(store.is_titled(id));

    fs::remove_all(dir);
}

TEST_CASE("titled flag round-trips through sqlite on reload") {
    const std::string dir = make_temp_dir();
    {
        ConversationStore store(dir);
        store.set_title_locked(store.active_id(), "locked title");
    }
    {
        ConversationStore store(dir);
        CHECK(store.is_titled(store.active_id()));
        CHECK(store.list().front().title == "locked title");
    }
    fs::remove_all(dir);
}

TEST_CASE("add_tokens persists total_tokens across store reloads") {
    const std::string dir = make_temp_dir();
    std::string id;
    {
        ConversationStore store(dir);
        id = store.active_id();
        store.add_tokens(id, 1200);
        store.add_tokens(id, 345);
        CHECK(store.list().front().total_tokens == 1545);
    }
    {
        ConversationStore store(dir);
        REQUIRE_FALSE(store.list().empty());
        CHECK(store.list().front().id == id);
        CHECK(store.list().front().total_tokens == 1545);
    }
    fs::remove_all(dir);
}

TEST_CASE("equal updated_at ties break by id so list order is deterministic") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string first = store.active_id();
    const std::string second = store.create(dir);
    const std::string third = store.create(dir);

    const auto entries = store.list();
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].id == third);
    CHECK(entries[1].id == second);
    CHECK(entries[2].id == first);
    for (size_t i = 1; i < entries.size(); ++i) {
        const bool ordered =
            entries[i - 1].updated_at > entries[i].updated_at ||
            (entries[i - 1].updated_at == entries[i].updated_at &&
             entries[i - 1].id > entries[i].id);
        CHECK(ordered);
    }

    fs::remove_all(dir);
}

namespace {

std::string session_with(const std::string& user, const std::string& assistant,
                         const std::string& agent_msg = {}) {
    std::ostringstream ss;
    ss << R"({"version":1,"index":[)"
       << R"({"role":"user","content":")" << user << R"("},)"
       << R"({"role":"assistant","content":")" << assistant << R"("}],)"
       << R"("agents":{)";
    if (!agent_msg.empty()) {
        ss << R"("scout":[{"role":"assistant","content":")" << agent_msg << R"("}])";
    }
    ss << "}}";
    return ss.str();
}

} // namespace

TEST_CASE("search finds text across conversations, case-insensitively") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string first = store.active_id();
    const std::string second = store.create(dir);
    write_session(store, first,
                  session_with("tune the flux capacitor",
                               "the Flux capacitor is tuned",
                               "flux readings nominal"));
    write_session(store, second, session_with("write a haiku", "done"));

    auto hits = store.search("FLUX");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == first);
    CHECK(hits[0].match_count == 3);
    CHECK(hits[0].snippet.find("flux") != std::string::npos);

    CHECK(store.search("no-such-text").empty());
    CHECK(store.search("").empty());

    fs::remove_all(dir);
}

TEST_CASE("search matches titles and skips deleted conversations") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    const std::string first = store.active_id();
    const std::string second = store.create(dir);
    write_session(store, first, session_with("hello", "world"));
    write_session(store, second, session_with("hello", "world"));
    store.set_title_locked(first, "flux notes");

    auto hits = store.search("flux");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == first);
    CHECK(hits[0].match_count == 1);

    hits = store.search("hello");
    CHECK(hits.size() == 2);
    store.soft_delete(second);
    hits = store.search("hello");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == first);

    fs::remove_all(dir);
}

TEST_CASE("tui and api conversations share tenants.db but sidebar lists tui only") {
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    const auto tid = store.tenant_id();
    store.tenant_store().create_conversation(tid, "HTTP thread", "index");

    CHECK(store.list().size() == 1);
    CHECK(store.list().front().title == "Untitled");

    fs::remove_all(dir);
}

TEST_CASE("migration is idempotent across a crash before the migrated flag") {
    const std::string dir = make_temp_dir();
    const std::string conv_dir = dir + "/conversations";
    fs::create_directories(conv_dir);
    {
        std::ofstream f(conv_dir + "/manifest.json");
        f << R"({"conversations":[{"id":"abc123","title":"Keep me","cwd":"/tmp",)"
          << R"("created_at":100,"updated_at":200,"total_tokens":7}]})";
    }
    {
        std::ofstream f(conv_dir + "/abc123.json");
        f << R"({"version":2,"index":[{"role":"user","content":"hi"}],)"
          << R"("agents":{},"compaction":{},"usage":{"total_tokens":42}})";
    }

    {
        ConversationStore store(dir);
        REQUIRE(store.list().size() == 1);
        CHECK(store.list().front().title == "Keep me");
        CHECK(store.list().front().total_tokens == 42); // session wins over manifest
        CHECK(store.list().front().created_at == 100);
        CHECK(store.list().front().updated_at == 200);
    }

    // Simulate crash after rows landed but before conversations_migrated=1.
    {
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open((dir + "/tenants.db").c_str(), &db) == SQLITE_OK);
        char* err = nullptr;
        REQUIRE(sqlite3_exec(db,
                             "UPDATE tui_prefs SET conversations_migrated = 0;",
                             nullptr, nullptr, &err) == SQLITE_OK);
        sqlite3_free(err);
        sqlite3_close(db);
    }

    {
        ConversationStore store(dir);
        CHECK(store.list().size() == 1); // no duplicate on resume
        CHECK(store.list().front().title == "Keep me");
        CHECK(store.list().front().total_tokens == 42);
        CHECK(store.tenant_store().tui_conversations_migrated(store.tenant_id()));
    }

    fs::remove_all(dir);
}

TEST_CASE("sessions/*.json import when conversations/ is absent") {
    const std::string dir = make_temp_dir();
    const std::string sessions = dir + "/sessions";
    fs::create_directories(sessions);
    {
        std::ofstream f(sessions + "/deadbeef.json");
        f << R"({"version":2,"index":[{"role":"user","content":"legacy"}],)"
          << R"("agents":{},"compaction":{}})";
    }

    ConversationStore store(dir);
    REQUIRE(store.list().size() == 1);
    CHECK(store.session_json(store.list().front().id).find("legacy")
          != std::string::npos);
    CHECK(store.tenant_store().tui_conversations_migrated(store.tenant_id()));

    fs::remove_all(dir);
}

TEST_CASE("sessions/*.conv tool scope remaps onto active TUI thread") {
    const std::string dir = make_temp_dir();
    fs::create_directories(dir + "/sessions");

    // Seed an API-origin "TUI session" row the way main.cpp used to, plus a
    // todo pinned to it, then point sessions/<cwd-hash>.conv at that id.
    int64_t legacy_tool_id = 0;
    {
        TenantStore tenants;
        tenants.open(dir + "/tenants.db");
        auto primary = tenants.create_tenant("default").tenant;
        auto api = tenants.create_conversation(primary.id, "TUI session", "index");
        legacy_tool_id = api.id;
        tenants.create_todo(primary.id, legacy_tool_id, "index",
                            "carry me", "");
        tenants.put_artifact(primary.id, legacy_tool_id, "note.txt",
                             "hello", "text/plain");

        std::string cwd = fs::current_path().string();
        std::uint32_t h = 2166136261u;
        for (unsigned char c : cwd) { h ^= c; h *= 16777619u; }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08x", h);
        std::ofstream f(dir + "/sessions/" + std::string(buf) + ".conv");
        f << legacy_tool_id << '\n';
    }

    ConversationStore store(dir);
    REQUIRE_FALSE(store.active_id().empty());
    const int64_t active = to_db_id(store.active_id());
    CHECK(active != legacy_tool_id);

    TenantStore::TodoFilter f;
    f.conversation_id = active;
    auto todos = store.tenant_store().list_todos(store.tenant_id(), f);
    REQUIRE(todos.size() == 1);
    CHECK(todos.front().subject == "carry me");

    auto arts = store.tenant_store().list_artifacts_conversation(
        store.tenant_id(), active, 50);
    REQUIRE(arts.size() == 1);
    CHECK(arts.front().path == "note.txt");

    CHECK_FALSE(store.tenant_store().get_conversation(
        store.tenant_id(), legacy_tool_id).has_value());

    // Idempotent: .conv is renamed so a reopen does not invent another thread.
    const auto before = store.list().size();
    {
        ConversationStore again(dir);
        CHECK(again.list().size() == before);
    }

    fs::remove_all(dir);
}

TEST_CASE("resolved_workspace_root uses conversation cwd not process cwd") {
    const std::string dir = make_temp_dir();
    const fs::path proj = fs::temp_directory_path() /
        ("arbiter_conv_ws_" + std::to_string(::getpid()));
    fs::create_directories(proj);

    ConversationStore store(dir);
    const std::string id = store.create(proj.string());
    REQUIRE_FALSE(id.empty());

    auto stored = store.cwd_of(id);
    REQUIRE(stored.has_value());
    CHECK(*stored == proj.string());

    const fs::path prev = fs::current_path();
    const fs::path decoy = fs::temp_directory_path() /
        ("arbiter_conv_decoy_" + std::to_string(::getpid()));
    fs::create_directories(decoy);
    fs::current_path(decoy);

    std::string err;
    const std::string root = store.resolved_workspace_root(id, &err);
    CHECK(err.empty());
    CHECK(fs::equivalent(root, proj));

    fs::remove_all(proj);
    err.clear();
    CHECK(store.resolved_workspace_root(id, &err).empty());
    CHECK(err.find("missing") != std::string::npos);

    fs::current_path(prev);
    fs::remove_all(decoy);
    fs::remove_all(dir);
}
