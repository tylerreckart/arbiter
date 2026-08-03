// tests/test_sandbox_quota.cpp — workspace quota under parallel /write (#129)
// and /exec mutex + post-check (#136).

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

// Minimal docker stub.  When ARBITER_TEST_WORKSPACE is set, `docker exec …
// sh -c <cmd>` runs <cmd> in that directory so /exec quota tests can mutate
// the bind-mounted workspace without a real daemon.
void install_docker_stub(const std::string& root) {
    const std::string bin = root + "/bin";
    fs::create_directories(bin);
    const std::string stub = bin + "/docker";
    {
        std::ofstream f(stub);
        f << R"(#!/bin/sh
if [ "$1" = exec ]; then
  shift
  cmd=""
  while [ $# -gt 0 ]; do
    if [ "$1" = -c ] && [ -n "$2" ]; then
      cmd="$2"
      break
    fi
    shift
  done
  if [ -n "$ARBITER_TEST_WORKSPACE" ] && [ -n "$cmd" ]; then
    cd "$ARBITER_TEST_WORKSPACE" && eval "$cmd"
    exit $?
  fi
fi
exit 0
)";
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

struct WorkspaceEnvGuard {
    const char* old_ws;
    explicit WorkspaceEnvGuard(const std::string& ws) {
        old_ws = std::getenv("ARBITER_TEST_WORKSPACE");
        ::setenv("ARBITER_TEST_WORKSPACE", ws.c_str(), 1);
    }
    ~WorkspaceEnvGuard() {
        if (old_ws) ::setenv("ARBITER_TEST_WORKSPACE", old_ws, 1);
        else ::unsetenv("ARBITER_TEST_WORKSPACE");
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

TEST_CASE("sandbox exec: post-check fails when shell write exceeds quota") {
    const std::string root = make_temp_root("quota-exec");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 7;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "seed.txt", std::string(700, 'a'), err));

    auto result = mgr.exec(
        tid, "head -c 400 /dev/zero > overflow.bin");
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("quota exceeded") != std::string::npos);
    CHECK(mgr.measure_workspace_bytes(tid) > kQuota);

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: oversized output includes truncation marker") {
    const std::string root = make_temp_root("sandbox-trunc");
    install_docker_stub(root);
    PathGuard path_guard(root);

    SandboxConfig cfg = make_quota_config(root, 0);
    cfg.output_max_bytes = 512;
    cfg.quota_check_pause_ms = 0;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 3;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    auto result = mgr.exec(tid, "head -c 4096 /dev/zero | tr '\\0' 'x'");
    CHECK(result.ok);
    CHECK(result.output.find("... [truncated at") != std::string::npos);
    CHECK(result.output.find(" KB]") != std::string::npos);

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: holds quota mutex so parallel /write cannot interleave") {
    const std::string root = make_temp_root("quota-exec-mutex");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;
    constexpr size_t kChunk = 200;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    cfg.quota_exec_pause_ms = 50;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 11;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    std::string err;
    REQUIRE(mgr.write_to_workspace(
        tid, "base.txt", std::string(kQuota - kChunk, 'b'), err));

    std::atomic<bool> write_finished{false};
    std::thread writer([&]() {
        std::string werr;
        write_finished.store(
            mgr.write_to_workspace(tid, "tail.txt", std::string(kChunk, 'c'), werr),
            std::memory_order_release);
    });

    auto result = mgr.exec(tid, "true");
    CHECK(result.ok);

    writer.join();
    CHECK(write_finished.load(std::memory_order_acquire));
    CHECK(mgr.measure_workspace_bytes(tid) == static_cast<int64_t>(kQuota));

    fs::remove_all(root);
}
