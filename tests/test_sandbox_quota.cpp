// tests/test_sandbox_quota.cpp — workspace quota under parallel /write (#129),
// /exec mutex + post-check (#136), and /exec over-cap rollback (#240).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "sandbox.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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
//
// `__ARB_TEST_OVERFLOW__` is a hermetic oversized-stdout seed (reads a
// pre-written sibling file) so truncation tests never depend on nested-quote
// shell generators or workspace visibility under the stub.
void install_docker_stub(const std::string& root) {
    const std::string bin = root + "/bin";
    fs::create_directories(bin);
    {
        std::ofstream overflow(bin + "/overflow.dat",
                               std::ios::binary | std::ios::trunc);
        overflow << std::string(4096, 'x');
        REQUIRE(overflow.good());
    }
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
  if [ -n "$cmd" ]; then
    # Exact match or wrapped by SandboxManager's timeout helper
    # (`if command -v timeout … sh -c '__ARB_TEST_OVERFLOW__'`).
    case "$cmd" in
      *__ARB_TEST_OVERFLOW__*)
        # Use $0 dirname expansion — not `dirname(1)` — because tests
        # put this stub's directory first on PATH and have no dirname there.
        here=${0%/*}
        /bin/cat "$here/overflow.dat"
        exit $?
        ;;
      *'/proc/[0-9]*'*)
        # Survivor cleanup walks /proc inside the container.  No-op on the
        # host stub so quota tests cannot SIGKILL CI processes.
        exit 0
        ;;
    esac
    if [ -n "$ARBITER_TEST_WORKSPACE" ]; then
      # Match real `docker exec … sh -c <cmd>` (not `eval`, which re-parses
      # quotes and broke nested generators on macOS CI). Absolute /bin/sh
      # so PATH shadows from the stub directory cannot hide the shell.
      cd "$ARBITER_TEST_WORKSPACE" && /bin/sh -c "$cmd"
      exit $?
    fi
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
    std::string old_path;
    bool had_path = false;
    explicit PathGuard(const std::string& root) {
        const char* p = std::getenv("PATH");
        had_path = p != nullptr;
        if (had_path) old_path = p;
        const std::string bin = root + "/bin";
        std::string new_path = bin + ":" + (had_path ? old_path : "");
        ::setenv("PATH", new_path.c_str(), 1);
    }
    ~PathGuard() {
        if (had_path) ::setenv("PATH", old_path.c_str(), 1);
        else ::unsetenv("PATH");
    }
};

struct WorkspaceEnvGuard {
    std::string old_ws;
    bool had_ws = false;
    explicit WorkspaceEnvGuard(const std::string& ws) {
        const char* p = std::getenv("ARBITER_TEST_WORKSPACE");
        had_ws = p != nullptr;
        if (had_ws) old_ws = p;
        ::setenv("ARBITER_TEST_WORKSPACE", ws.c_str(), 1);
    }
    ~WorkspaceEnvGuard() {
        if (had_ws) ::setenv("ARBITER_TEST_WORKSPACE", old_ws.c_str(), 1);
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
    CHECK(result.error.find("rolled back") != std::string::npos);
    CHECK(mgr.measure_workspace_bytes(tid) <= kQuota);
    CHECK(mgr.measure_workspace_bytes(tid) == 700);
    CHECK_FALSE(fs::exists(fs::path(ws) / "overflow.bin"));
    CHECK(fs::file_size(fs::path(ws) / "seed.txt") == 700);

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: quota rollback truncates appended files") {
    const std::string root = make_temp_root("quota-exec-append");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 8;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "seed.txt", std::string(700, 'a'), err));

    auto result = mgr.exec(
        tid, "head -c 400 /dev/zero >> seed.txt");
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("rolled back") != std::string::npos);
    CHECK(mgr.measure_workspace_bytes(tid) == 700);
    CHECK(fs::file_size(fs::path(ws) / "seed.txt") == 700);
    {
        std::ifstream in(fs::path(ws) / "seed.txt", std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        CHECK(body == std::string(700, 'a'));
    }

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: quota rollback restores files deleted by the command") {
    const std::string root = make_temp_root("quota-exec-delete");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 12;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "seed.txt", std::string(700, 'a'), err));

    auto result = mgr.exec(
        tid, "rm seed.txt; head -c 1100 /dev/zero > overflow.bin");
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("rolled back") != std::string::npos);
    CHECK(mgr.measure_workspace_bytes(tid) == 700);
    CHECK(fs::exists(fs::path(ws) / "seed.txt"));
    CHECK_FALSE(fs::exists(fs::path(ws) / "overflow.bin"));
    {
        std::ifstream in(fs::path(ws) / "seed.txt", std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        CHECK(body == std::string(700, 'a'));
    }

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: quota rollback restores same-size overwrites") {
    const std::string root = make_temp_root("quota-exec-overwrite");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 13;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "seed.txt", std::string(700, 'a'), err));

    auto result = mgr.exec(
        tid, "head -c 700 /dev/zero | tr '\\0' 'b' > seed.txt; "
             "head -c 400 /dev/zero > overflow.bin");
    CHECK_FALSE(result.ok);
    CHECK(mgr.measure_workspace_bytes(tid) == 700);
    {
        std::ifstream in(fs::path(ws) / "seed.txt", std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        CHECK(body == std::string(700, 'a'));
    }
    CHECK_FALSE(fs::exists(fs::path(ws) / "overflow.bin"));

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: cancel restores under-quota mutations") {
    const std::string root = make_temp_root("quota-exec-cancel");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 14;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "seed.txt", std::string(100, 'a'), err));

    std::atomic<bool> cancel{false};
    std::thread canceller([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancel.store(true, std::memory_order_release);
    });
    auto result = mgr.exec(
        tid, "head -c 50 /dev/zero > partial.bin; sleep 5",
        /*timeout_seconds_override=*/0, &cancel);
    canceller.join();

    CHECK(result.canceled);
    CHECK_FALSE(result.ok);
    CHECK(result.output.find("[cancelled]") != std::string::npos);
    CHECK(result.output.find("ERR:") != std::string::npos);
    CHECK_FALSE(fs::exists(fs::path(ws) / "partial.bin"));
    CHECK(fs::exists(fs::path(ws) / "seed.txt"));
    CHECK(mgr.measure_workspace_bytes(tid) == 100);

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: SIGKILL status 137 before the deadline keeps writes") {
    const std::string root = make_temp_root("quota-exec-137");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    cfg.exec_timeout_seconds = 30;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 21;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "seed.txt", std::string(100, 'a'), err));

    // Command exits 137 immediately (OOM / self-kill), far short of the
    // 30s deadline.  Must not be classified as a wrapper timeout.
    auto result = mgr.exec(
        tid, "head -c 50 /dev/zero > kept.bin; exit 137");
    CHECK_FALSE(result.timed_out);
    CHECK_FALSE(result.canceled);
    CHECK(result.output.find("[timed out") == std::string::npos);
    CHECK(result.output.find("[exit 137]") != std::string::npos);
    CHECK(fs::exists(fs::path(ws) / "kept.bin"));
    CHECK(fs::file_size(fs::path(ws) / "kept.bin") == 50);
    CHECK(fs::exists(fs::path(ws) / "seed.txt"));
    CHECK(mgr.measure_workspace_bytes(tid) == 150);

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: SIGKILL status 137 near the deadline keeps writes") {
    const std::string root = make_temp_root("quota-exec-137-late");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 24;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "seed.txt", std::string(100, 'a'), err));

    // Finishes ~200ms before a 2s deadline with 137.  A wall-clock
    // heuristic would still classify this as timeout; the wrapper
    // did not fire, so the write must stay.
    auto result = mgr.exec(
        tid, "head -c 50 /dev/zero > kept.bin; sleep 1.8; exit 137",
        /*timeout_seconds_override=*/2);
    CHECK_FALSE(result.timed_out);
    CHECK_FALSE(result.canceled);
    CHECK(result.output.find("[timed out") == std::string::npos);
    CHECK(result.output.find("[exit 137]") != std::string::npos);
    CHECK(fs::exists(fs::path(ws) / "kept.bin"));
    CHECK(fs::file_size(fs::path(ws) / "kept.bin") == 50);
    CHECK(mgr.measure_workspace_bytes(tid) == 150);

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: SIGTERM status 143 before the deadline keeps writes") {
    const std::string root = make_temp_root("quota-exec-143");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    cfg.exec_timeout_seconds = 30;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 22;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "seed.txt", std::string(100, 'a'), err));

    auto result = mgr.exec(
        tid, "head -c 50 /dev/zero > kept.bin; exit 143");
    CHECK_FALSE(result.timed_out);
    CHECK(result.output.find("[timed out") == std::string::npos);
    CHECK(result.output.find("[exit 143]") != std::string::npos);
    CHECK(fs::exists(fs::path(ws) / "kept.bin"));
    CHECK(fs::file_size(fs::path(ws) / "kept.bin") == 50);
    CHECK(mgr.measure_workspace_bytes(tid) == 150);

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: natural status 124 is not a timeout") {
    const std::string root = make_temp_root("quota-exec-124");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    cfg.exec_timeout_seconds = 30;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 23;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "seed.txt", std::string(100, 'a'), err));

    auto result = mgr.exec(
        tid, "head -c 50 /dev/zero > kept.bin; exit 124");
    CHECK_FALSE(result.timed_out);
    CHECK(result.output.find("[timed out") == std::string::npos);
    CHECK(result.output.find("[exit 124]") != std::string::npos);
    CHECK(fs::exists(fs::path(ws) / "kept.bin"));
    CHECK(fs::file_size(fs::path(ws) / "kept.bin") == 50);
    CHECK(mgr.measure_workspace_bytes(tid) == 150);
    // Watchdog stamp (and its atomic-write .tmp) must not leak.
    bool leaked = false;
    for (auto it = fs::directory_iterator(ws); it != fs::directory_iterator(); ++it) {
        const auto name = it->path().filename().string();
        if (name.rfind(".arbiter-to-", 0) == 0)
            leaked = true;
    }
    CHECK_FALSE(leaked);

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: timeout restores under-quota mutations") {
    const std::string root = make_temp_root("quota-exec-timeout");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 15;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "seed.txt", std::string(100, 'a'), err));

    // Watchdog writes the stamp before SIGKILL so leftover reap of the
    // watchdog cannot drop a real timeout (parent would otherwise see
    // ordinary 137 and keep this under-quota write).
    auto result = mgr.exec(
        tid, "head -c 50 /dev/zero > partial.bin; sleep 5",
        /*timeout_seconds_override=*/1);
    CHECK(result.timed_out);
    CHECK(result.output.find("[timed out") != std::string::npos);
    CHECK_FALSE(fs::exists(fs::path(ws) / "partial.bin"));
    CHECK(fs::exists(fs::path(ws) / "seed.txt"));
    CHECK(mgr.measure_workspace_bytes(tid) == 100);
    bool leaked = false;
    for (auto it = fs::directory_iterator(ws); it != fs::directory_iterator(); ++it) {
        if (it->path().filename().string().rfind(".arbiter-to-", 0) == 0)
            leaked = true;
    }
    CHECK_FALSE(leaked);

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: quota rollback removes chmod-locked overflow") {
    const std::string root = make_temp_root("quota-exec-chmod");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 15;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "seed.txt", std::string(700, 'a'), err));

    auto result = mgr.exec(
        tid, "head -c 400 /dev/zero > overflow.bin; chmod 000 overflow.bin");
    CHECK_FALSE(result.ok);
    CHECK(mgr.measure_workspace_bytes(tid) == 700);
    CHECK_FALSE(fs::exists(fs::path(ws) / "overflow.bin"));

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: quota rollback does not follow dest parent symlinks") {
    const std::string root = make_temp_root("quota-exec-symlink");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 16;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    const fs::path outside = fs::path(root) / "outside";
    fs::create_directories(outside);

    std::string err;
    REQUIRE(mgr.write_to_workspace(
        tid, "keep/nested/seed.txt", std::string(700, 'a'), err));

    // Replace the intermediate directory with a symlink to a host path
    // outside the workspace, then overflow.  Leaf O_NOFOLLOW would still
    // write keep/nested/seed.txt through that parent.
    auto result = mgr.exec(
        tid,
        "rm -rf keep/nested; ln -s " + outside.string() + " keep/nested; "
        "echo hijacked > keep/nested/seed.txt; "
        "head -c 1100 /dev/zero > overflow.bin");
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("rolled back") != std::string::npos);

    const fs::path keep_nested = fs::path(ws) / "keep" / "nested";
    CHECK_FALSE(fs::is_symlink(keep_nested));
    CHECK(fs::is_directory(keep_nested));
    {
        std::ifstream in(keep_nested / "seed.txt", std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        CHECK(body == std::string(700, 'a'));
    }
    CHECK_FALSE(fs::exists(fs::path(ws) / "overflow.bin"));
    CHECK(mgr.measure_workspace_bytes(tid) == 700);

    {
        std::ifstream in(outside / "seed.txt");
        std::string body;
        std::getline(in, body);
        CHECK(body == "hijacked");
    }

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: quota rollback replaces a top-level dest symlink") {
    const std::string root = make_temp_root("quota-exec-symlink-root");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 17;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    const fs::path outside = fs::path(root) / "outside";
    fs::create_directories(outside);

    std::string err;
    REQUIRE(mgr.write_to_workspace(
        tid, "keep/nested/seed.txt", std::string(700, 'a'), err));

    auto result = mgr.exec(
        tid,
        "rm -rf keep; ln -s " + outside.string() + " keep; "
        "mkdir -p keep/nested; echo hijacked > keep/nested/seed.txt; "
        "head -c 1100 /dev/zero > overflow.bin");
    CHECK_FALSE(result.ok);

    const fs::path keep = fs::path(ws) / "keep";
    CHECK_FALSE(fs::is_symlink(keep));
    CHECK(fs::is_directory(keep));
    {
        std::ifstream in(keep / "nested" / "seed.txt", std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        CHECK(body == std::string(700, 'a'));
    }
    {
        std::ifstream in(outside / "nested" / "seed.txt");
        std::string body;
        std::getline(in, body);
        CHECK(body == "hijacked");
    }

    fs::remove_all(root);
}

TEST_CASE("sandbox exec: writes under quota are kept") {
    const std::string root = make_temp_root("quota-exec-keep");
    install_docker_stub(root);
    PathGuard path_guard(root);
    constexpr int64_t kQuota = 1000;

    SandboxConfig cfg = make_quota_config(root, kQuota);
    cfg.quota_check_pause_ms = 0;
    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());
    const int64_t tid = 9;
    const std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());
    WorkspaceEnvGuard ws_env(ws);

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "seed.txt", std::string(100, 'a'), err));

    auto result = mgr.exec(tid, "head -c 50 /dev/zero > ok.bin");
    CHECK(result.ok);
    CHECK(mgr.measure_workspace_bytes(tid) == 150);
    CHECK(fs::exists(fs::path(ws) / "ok.bin"));
    CHECK(fs::file_size(fs::path(ws) / "ok.bin") == 50);

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
    REQUIRE_FALSE(mgr.ensure_workspace(tid).empty());

    // Stub-local overflow seed — no workspace bind, no nested quotes, no
    // python/awk.  macOS CI repeatedly collapsed every in-container generator
    // (and even `cat` of a /write'd blob) to a few dozen bytes.
    auto result = mgr.exec(tid, "__ARB_TEST_OVERFLOW__");
    INFO("exec output (" << result.output.size() << " bytes): " << result.output);
    CHECK(result.ok);
    CHECK(result.output.size() >= 512);
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
