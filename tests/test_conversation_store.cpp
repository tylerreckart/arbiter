#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "atomic_file.h"
#include "repl/conversation_store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
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
        t.detach(); // already failed; avoid blocking process teardown on a hung thread
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

    // Overwrite: still atomic, still no leftover tmp file.
    CHECK(atomic_write_file(path, "second write"));
    CHECK(read_all(path) == "second write");
    CHECK_FALSE(fs::exists(path + ".tmp"));

    fs::remove_all(dir);
}

TEST_CASE("corrupt manifest is recovered by scanning session files, not cleared") {
    const std::string dir = make_temp_dir();
    const std::string conv_dir = dir + "/conversations";
    fs::create_directories(conv_dir);

    // Corrupt manifest.
    {
        std::ofstream f(conv_dir + "/manifest.json");
        f << "{ this is not valid json";
    }
    // One orphaned-but-real session file that the corrupt manifest would
    // otherwise silently disown.
    {
        std::ofstream f(conv_dir + "/deadbeefcafebabe.json");
        f << R"({"version":1,"index":[{"role":"user","content":"hi"}],"agents":{}})";
    }

    ConversationStore store(dir);
    const auto entries = store.list();

    bool found = false;
    for (const auto& e : entries) {
        if (e.id == "deadbeefcafebabe") {
            found = true;
            CHECK(e.title == "Untitled (recovered)");
        }
    }
    CHECK(found);

    fs::remove_all(dir);
}

TEST_CASE("soft delete filters list() but keeps the session file; purge removes it") {
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
    CHECK(fs::exists(store.session_path(a)));
    CHECK(read_all(dir + "/conversations/manifest.json").find("deleted_at") != std::string::npos);

    store.purge(b);
    CHECK_FALSE(fs::exists(store.session_path(b)));
    CHECK(read_all(dir + "/conversations/manifest.json").find(b) == std::string::npos);

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

    // Once locked, a plain set_title (as the deterministic path would issue
    // on a later, unrelated turn) must not silently unlock it.
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
    store.lock_title(id); // simulates the model-title job failing/timing out
    CHECK(store.list().front().title == "deterministic title");
    CHECK(store.is_titled(id));

    fs::remove_all(dir);
}

TEST_CASE("titled flag round-trips through the manifest on disk") {
    const std::string dir = make_temp_dir();
    {
        ConversationStore store(dir);
        store.set_title_locked(store.active_id(), "locked title");
    }
    {
        ConversationStore store(dir); // fresh instance re-reads manifest.json
        CHECK(store.is_titled(store.active_id()));
        CHECK(store.list().front().title == "locked title");
    }
    fs::remove_all(dir);
}
