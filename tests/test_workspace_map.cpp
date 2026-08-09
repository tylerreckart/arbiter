// tests/test_workspace_map.cpp — /map tree walker (cmd_map only).
// Parse/dispatch coverage lives in test_commands.cpp (links commands.cpp).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "workspace_map.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace arbiter;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        const auto pid = static_cast<long long>(::getpid());
        const auto now = std::chrono::steady_clock::now()
                              .time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("arbiter_map_" + std::to_string(pid) + "_" +
                std::to_string(now));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void touch(const fs::path& p, const std::string& body = "x\n") {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << body;
}

} // namespace

TEST_CASE("cmd_map renders tree and skips heavy dirs") {
    TempDir dir;
    touch(dir.path / "README.md");
    touch(dir.path / "src" / "main.cpp");
    touch(dir.path / "include" / "foo.h");
    touch(dir.path / "node_modules" / "pkg" / "index.js");
    touch(dir.path / ".git" / "config");
    touch(dir.path / ".github" / "workflows" / "ci.yml");

    auto out = cmd_map(dir.path.string());
    CHECK(out.find("ERR:") != 0);
    CHECK(out.find("README.md") != std::string::npos);
    CHECK(out.find("src/") != std::string::npos);
    CHECK(out.find("main.cpp") != std::string::npos);
    CHECK(out.find("include/") != std::string::npos);
    CHECK(out.find(".github/") != std::string::npos);
    CHECK(out.find("node_modules") == std::string::npos);
    CHECK(out.find(".git/") == std::string::npos);
}

TEST_CASE("cmd_map scopes to subdirectory and refuses escape") {
    TempDir dir;
    touch(dir.path / "src" / "a.cpp");
    touch(dir.path / "other" / "b.cpp");

    auto scoped = cmd_map(dir.path.string(), "src");
    CHECK(scoped.find("ERR:") != 0);
    CHECK(scoped.find("a.cpp") != std::string::npos);
    CHECK(scoped.find("b.cpp") == std::string::npos);

    CHECK(cmd_map(dir.path.string(), "../etc").find("ERR:") == 0);
    CHECK(cmd_map(dir.path.string(), "/tmp").find("ERR:") == 0);
    CHECK(cmd_map(dir.path.string(), "missing").find("ERR:") == 0);
}

TEST_CASE("cmd_map honors entry cap truncation") {
    TempDir dir;
    for (int i = 0; i < 20; ++i) {
        touch(dir.path / ("f" + std::to_string(i) + ".txt"));
    }
    WorkspaceMapOptions opts;
    opts.max_entries = 5;
    opts.max_depth = 2;
    opts.max_bytes = 0;
    auto out = cmd_map(dir.path.string(), {}, opts);
    CHECK(out.find("truncated") != std::string::npos);
    CHECK(out.find("entry cap") != std::string::npos);
}

TEST_CASE("cmd_map refuses missing workspace root") {
    TempDir decoy;
    const fs::path missing = decoy.path / "gone";
    const fs::path prev = fs::current_path();
    fs::current_path(decoy.path);
    auto out = cmd_map(missing.string());
    CHECK(out.find("ERR:") == 0);
    fs::current_path(prev);
}
