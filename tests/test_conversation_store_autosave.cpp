#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "orchestrator.h"
#include "repl/conversation_store.h"

#include <chrono>
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

} // namespace

TEST_CASE("save_async persists and flush() blocks until it lands") {
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
    const std::string dir = make_temp_dir();
    ConversationStore store(dir);

    store.flush();
    store.flush();
    CHECK(true); // reaching here means flush() didn't hang with no pending work

    fs::remove_all(dir);
}
