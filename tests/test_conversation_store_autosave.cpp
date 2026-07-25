#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "orchestrator.h"
#include "repl/conversation_store.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
    name << "arbiter_autosave_test_" << rd() << "_" << counter++;
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

struct EnvVar {
    std::string key;
    explicit EnvVar(std::string k, const char* value) : key(std::move(k)) {
        ::setenv(key.c_str(), value, 1);
    }
    ~EnvVar() { ::unsetenv(key.c_str()); }
};

std::int64_t updated_at_for(ConversationStore& store, const std::string& id) {
    for (const auto& e : store.list()) {
        if (e.id == id) return e.updated_at;
    }
    return 0;
}

} // namespace

TEST_CASE("save_async persists and flush() blocks until it lands") {
    EnvVar interval("ARBITER_AUTOSAVE_INTERVAL_SEC", "0");
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    Orchestrator orch({});

    const std::string id = store.active_id();
    const std::int64_t before_updated_at = store.list().front().updated_at;

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    store.save_async(id, orch);
    store.flush();

    const auto entries = store.list();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().updated_at > before_updated_at);

    fs::remove_all(dir);
}

TEST_CASE("save_async coalesces a burst into the latest save") {
    EnvVar interval("ARBITER_AUTOSAVE_INTERVAL_SEC", "0");
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    Orchestrator orch({});

    const std::string id = store.active_id();

    // Fire a burst of save_async calls back-to-back — the "latest wins"
    // pending slot should coalesce these rather than queue N saves.
    for (int i = 0; i < 25; ++i) {
        store.save_async(id, orch);
    }
    store.flush();

    // The session file must reflect a completed, non-corrupt save (valid
    // JSON), not a torn write from an overlapping save.
    const std::string raw = read_all(store.session_path(id));
    CHECK_FALSE(raw.empty());
    CHECK(raw.find("\"version\"") != std::string::npos);

    fs::remove_all(dir);
}

TEST_CASE("flush() is a no-op when nothing is pending") {
    EnvVar interval("ARBITER_AUTOSAVE_INTERVAL_SEC", "0");
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    store.flush();
    store.flush();
    CHECK(true); // reaching here means flush() didn't hang with no pending work

    fs::remove_all(dir);
}

TEST_CASE("save_async keeps a pending slot per conversation id") {
    EnvVar interval("ARBITER_AUTOSAVE_INTERVAL_SEC", "0");
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    Orchestrator orch({});

    const std::string id_a = store.active_id();
    const std::string id_b = store.create(fs::current_path().string());
    REQUIRE(id_a != id_b);

    const std::int64_t before_a = updated_at_for(store, id_a);
    const std::int64_t before_b = updated_at_for(store, id_b);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    // A single global pending slot would drop one of these.
    store.save_async(id_a, orch);
    store.save_async(id_b, orch);
    store.flush();

    CHECK(updated_at_for(store, id_a) > before_a);
    CHECK(updated_at_for(store, id_b) > before_b);

    fs::remove_all(dir);
}

TEST_CASE("mark_dirty is persisted by the periodic autosave tick") {
    EnvVar interval("ARBITER_AUTOSAVE_INTERVAL_SEC", "1");
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);
    Orchestrator orch({});
    CHECK(store.autosave_interval() == std::chrono::seconds(1));

    const std::string id = store.active_id();
    const std::int64_t before = updated_at_for(store, id);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    store.mark_dirty(id, orch);

    // Wait for the 1s worker tick to promote dirty → save; flush drains
    // anything still in flight.
    bool landed = false;
    for (int i = 0; i < 40 && !landed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        landed = updated_at_for(store, id) > before;
    }
    store.flush();
    CHECK(updated_at_for(store, id) > before);

    fs::remove_all(dir);
}

TEST_CASE("autosave_interval_from_env parses overrides") {
    {
        EnvVar interval("ARBITER_AUTOSAVE_INTERVAL_SEC", "45");
        CHECK(ConversationStore::autosave_interval_from_env() ==
              std::chrono::seconds(45));
    }
    {
        EnvVar interval("ARBITER_AUTOSAVE_INTERVAL_SEC", "0");
        CHECK(ConversationStore::autosave_interval_from_env() ==
              std::chrono::seconds(0));
    }
    {
        EnvVar interval("ARBITER_AUTOSAVE_INTERVAL_SEC", "-3");
        CHECK(ConversationStore::autosave_interval_from_env() ==
              std::chrono::seconds(30));
    }
}
