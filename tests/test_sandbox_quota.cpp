// tests/test_sandbox_quota.cpp — workspace quota under parallel /write (#129).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "sandbox.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace arbiter;

namespace {

std::string make_temp_root(const char* tag) {
    auto base = fs::temp_directory_path() /
                (std::string("arbiter-") + tag + "-XXXXXX");
    std::string tmpl = base.string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* dir = ::mkdtemp(buf.data());
    REQUIRE(dir != nullptr);
    return std::string(dir);
}

void install_docker_stub(const std::string& root) {
    const std::string bin = root + "/bin";
    fs::create_directories(bin);
    const std::string stub = bin + "/docker";
    {
        std::ofstream f(stub);
        f << "#!/bin/sh\nexit 0\n";
    }
    ::chmod(stub.c_str(), 0755);
}

SandboxConfig make_quota_config(const std::string& root, int64_t quota) {
    SandboxConfig cfg;
    cfg.image = "unused";
    cfg.workspaces_root = root + "/workspaces";
    cfg.runtime = "docker";
    cfg.idle_seconds = 0;
    cfg.workspace_max_bytes = quota;
    cfg.quota_check_pause_ms = 50;
    return cfg;
}

struct PathGuard {
    const char* old_path;
    explicit PathGuard(const std::string& root) {
        const std::string bin = root + "/bin";
        old_path = std::getenv("PATH");
        std::string new_path = bin + ":" + (old_path ? old_path : "");
        ::setenv("PATH", new_path.c_str(), 1);
    }
    ~PathGuard() {
        if (old_path) ::setenv("PATH", old_path, 1);
        else ::unsetenv("PATH");
    }
};

} // namespace

TEST_CASE("sandbox write_to_workspace: parallel writers respect quota") {
    const std::string root = make_temp_root("quota");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;
    constexpr size_t kThreads = 8;
    constexpr size_t kChunk = 200;  // 8 * 200 = 1600 > 1000

    SandboxManager mgr(make_quota_config(root, kQuota));
    REQUIRE(mgr.usable());
    const int64_t tid = 42;
    REQUIRE_FALSE(mgr.ensure_workspace(tid).empty());

    std::atomic<int> successes{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            std::string err;
            const std::string path = "file_" + std::to_string(i) + ".txt";
            const std::string body(kChunk, static_cast<char>('a' + (i % 26)));
            if (mgr.write_to_workspace(tid, path, body, err))
                successes.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& t : threads) t.join();

    CHECK(mgr.measure_workspace_bytes(tid) <= kQuota);
    CHECK(successes.load() == static_cast<int>(kQuota / kChunk));

    fs::remove_all(root);
}

TEST_CASE("sandbox write_to_workspace: in-place overwrite charges delta only") {
    const std::string root = make_temp_root("quota-overwrite");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 500;

    SandboxManager mgr(make_quota_config(root, kQuota));
    REQUIRE(mgr.usable());
    const int64_t tid = 3;

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "data.txt", std::string(300, 'x'), err));
    CHECK(mgr.measure_workspace_bytes(tid) == 300);

    // Grow by 100 bytes — should succeed (400 total).
    REQUIRE(mgr.write_to_workspace(tid, "data.txt", std::string(400, 'y'), err));
    CHECK(mgr.measure_workspace_bytes(tid) == 400);

    // Would exceed quota by 200 bytes.
    err.clear();
    CHECK_FALSE(mgr.write_to_workspace(tid, "data.txt", std::string(700, 'z'), err));
    CHECK(err.find("quota exceeded") != std::string::npos);

    fs::remove_all(root);
}
