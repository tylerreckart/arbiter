// tests/test_sandbox_ssrf.cpp — workspace symlink escape + SSRF denylist
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "sandbox.h"
#include "ssrf_guard.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace arbiter;

namespace {

std::string make_temp_root(const char* tag) {
    auto base = fs::temp_directory_path() / (std::string("arbiter-") + tag + "-XXXXXX");
    std::string tmpl = base.string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* dir = ::mkdtemp(buf.data());
    REQUIRE(dir != nullptr);
    return std::string(dir);
}

} // namespace

TEST_CASE("ssrf_is_blocked_v4 covers loopback RFC1918 link-local CGNAT") {
    CHECK(ssrf_is_blocked_v4(0x7f000001u));           // 127.0.0.1
    CHECK(ssrf_is_blocked_v4(0x0a000001u));           // 10.0.0.1
    CHECK(ssrf_is_blocked_v4(0xc0a80001u));           // 192.168.0.1
    CHECK(ssrf_is_blocked_v4(0xac100001u));           // 172.16.0.1
    CHECK(ssrf_is_blocked_v4(0xa9fea9feu));           // 169.254.169.254
    CHECK(ssrf_is_blocked_v4(0x64400001u));           // 100.64.0.1 CGNAT
    CHECK_FALSE(ssrf_is_blocked_v4(0x08080808u));    // 8.8.8.8
}

TEST_CASE("ssrf_is_blocked_v6 covers loopback mapped and ULA") {
    struct in6_addr a{};
    CHECK(inet_pton(AF_INET6, "::1", &a) == 1);
    CHECK(ssrf_is_blocked_v6(&a));

    CHECK(inet_pton(AF_INET6, "::ffff:127.0.0.1", &a) == 1);
    CHECK(ssrf_is_blocked_v6(&a));

    CHECK(inet_pton(AF_INET6, "fd00::1", &a) == 1);
    CHECK(ssrf_is_blocked_v6(&a));

    CHECK(inet_pton(AF_INET6, "2001:4860:4860::8888", &a) == 1);
    CHECK_FALSE(ssrf_is_blocked_v6(&a));
}

TEST_CASE("ssrf_preflight_url rejects loopback literals and metadata hosts") {
    CHECK(ssrf_preflight_url("http://127.0.0.1/").find("blocked") != std::string::npos);
    CHECK(ssrf_preflight_url("http://[::1]/").find("blocked") != std::string::npos);
    CHECK(ssrf_preflight_url("http://metadata.google.internal/")
              .find("denylist") != std::string::npos);
    CHECK(ssrf_preflight_url("ftp://example.com/") ==
          "URL must start with http:// or https://");
}

TEST_CASE("sandbox write/read reject symlink escape outside workspace") {
    const std::string root = make_temp_root("ws");
    const std::string outside = root + "/outside.txt";
    {
        std::ofstream f(outside);
        f << "secret-host-bytes";
    }

    SandboxConfig cfg;
    cfg.image = "unused";                 // usable_ needs non-empty image
    cfg.workspaces_root = root + "/workspaces";
    cfg.runtime = "docker";               // may be missing — force usable path
    // Path-only unit test: do not start the idle reaper.  A live reaper
    // thread + stack-allocated SandboxManager races TSan (false "double
    // lock" on mu_ during wait_for vs touch_access).
    cfg.idle_seconds = 0;
    // Construct then poke usable_ indirectly: write_to_workspace checks usable_.
    // When docker isn't on PATH, usable_ is false.  Bypass by writing through
    // a manager that we mark usable via ensuring image+root only... Looking
    // at ctor: usable_ requires runtime on PATH.  For unit tests without
    // docker, call the path helpers by constructing with a fake: we still
    // need usable_=true.
    //
    // Work around: create directories ourselves and use write/read which
    // early-return on !usable_.  So skip if docker missing OR force by
    // temporarily putting a stub `docker` on PATH.
    const std::string bin = root + "/bin";
    fs::create_directories(bin);
    const std::string stub = bin + "/docker";
    {
        std::ofstream f(stub);
        f << "#!/bin/sh\nexit 0\n";
    }
    ::chmod(stub.c_str(), 0755);
    const char* old_path = std::getenv("PATH");
    std::string new_path = bin + ":" + (old_path ? old_path : "");
    ::setenv("PATH", new_path.c_str(), 1);

    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());

    const int64_t tid = 7;
    std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());

    // Plant a symlink inside the workspace pointing at the host file.
    const std::string link = ws + "/leak";
    REQUIRE(::symlink(outside.c_str(), link.c_str()) == 0);

    std::string err;
    CHECK_FALSE(mgr.write_to_workspace(tid, "leak", "pwned", err));
    CHECK(err.find("escapes") != std::string::npos);

    // Host file must be unchanged.
    {
        std::ifstream f(outside);
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        CHECK(body == "secret-host-bytes");
    }

    std::string content, mime;
    err.clear();
    CHECK_FALSE(mgr.read_from_workspace(tid, "leak", content, mime, err));
    CHECK(err.find("escapes") != std::string::npos);

    // In-workspace relative symlink to a sibling file should still work.
    {
        std::string werr;
        REQUIRE(mgr.write_to_workspace(tid, "ok.txt", "hello", werr));
        const std::string sib = ws + "/alias";
        REQUIRE(::symlink("ok.txt", sib.c_str()) == 0);
        content.clear();
        mime.clear();
        err.clear();
        REQUIRE(mgr.read_from_workspace(tid, "alias", content, mime, err));
        CHECK(content == "hello");
    }

    if (old_path) ::setenv("PATH", old_path, 1);
    else ::unsetenv("PATH");
    fs::remove_all(root);
}

TEST_CASE("sandbox list/measure skip directory symlinks outside workspace") {
    const std::string root = make_temp_root("listsym");
    const std::string outside = root + "/outside";
    fs::create_directories(outside);
    {
        std::ofstream f(outside + "/secret.txt");
        f << "leaked";
    }

    SandboxConfig cfg;
    cfg.image = "unused";
    cfg.workspaces_root = root + "/workspaces";
    cfg.runtime = "docker";
    cfg.idle_seconds = 0;

    const std::string bin = root + "/bin";
    fs::create_directories(bin);
    const std::string stub = bin + "/docker";
    {
        std::ofstream f(stub);
        f << "#!/bin/sh\nexit 0\n";
    }
    ::chmod(stub.c_str(), 0755);
    const char* old_path = std::getenv("PATH");
    std::string new_path = bin + ":" + (old_path ? old_path : "");
    ::setenv("PATH", new_path.c_str(), 1);

    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());

    const int64_t tid = 20;
    std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());

    std::string err;
    REQUIRE(mgr.write_to_workspace(tid, "ok.txt", "safe", err));

    const std::string link = ws + "/escape";
    REQUIRE(::symlink(outside.c_str(), link.c_str()) == 0);

    const std::string listing = mgr.list_workspace(tid);
    CHECK(listing.find("secret.txt") == std::string::npos);
    CHECK(listing.find("ok.txt") != std::string::npos);
    CHECK(mgr.measure_workspace_bytes(tid) == static_cast<int64_t>(4));

    if (old_path) ::setenv("PATH", old_path, 1);
    else ::unsetenv("PATH");
    fs::remove_all(root);
}

TEST_CASE("sandbox read: rejects workspace files over read_max_bytes") {
    const std::string root = make_temp_root("readcap");
    SandboxConfig cfg;
    cfg.image = "unused";
    cfg.workspaces_root = root + "/workspaces";
    cfg.runtime = "docker";
    cfg.idle_seconds = 0;
    cfg.read_max_bytes = 512;

    const std::string bin = root + "/bin";
    fs::create_directories(bin);
    const std::string stub = bin + "/docker";
    {
        std::ofstream f(stub);
        f << "#!/bin/sh\nexit 0\n";
    }
    ::chmod(stub.c_str(), 0755);
    const char* old_path = std::getenv("PATH");
    std::string new_path = bin + ":" + (old_path ? old_path : "");
    ::setenv("PATH", new_path.c_str(), 1);

    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());

    const int64_t tid = 11;
    std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());

    const std::string path = ws + "/big.bin";
    {
        std::ofstream f(path, std::ios::binary);
        f << std::string(700, 'x');
    }

    std::string content, mime, err;
    CHECK_FALSE(mgr.read_from_workspace(tid, "big.bin", content, mime, err));
    CHECK(err.find("too large") != std::string::npos);

    err.clear();
    REQUIRE(mgr.write_to_workspace(tid, "small.txt", "hi", err));
    content.clear();
    err.clear();
    REQUIRE(mgr.read_from_workspace(tid, "small.txt", content, mime, err));
    CHECK(content == "hi");

    if (old_path) ::setenv("PATH", old_path, 1);
    else ::unsetenv("PATH");
    fs::remove_all(root);
}

TEST_CASE("sandbox list: caps workspace listing output") {
    const std::string root = make_temp_root("listcap");
    SandboxConfig cfg;
    cfg.image = "unused";
    cfg.workspaces_root = root + "/workspaces";
    cfg.runtime = "docker";
    cfg.idle_seconds = 0;
    cfg.list_max_bytes = 64;
    cfg.list_max_files = 0;

    const std::string bin = root + "/bin";
    fs::create_directories(bin);
    const std::string stub = bin + "/docker";
    {
        std::ofstream f(stub);
        f << "#!/bin/sh\nexit 0\n";
    }
    ::chmod(stub.c_str(), 0755);
    const char* old_path = std::getenv("PATH");
    std::string new_path = bin + ":" + (old_path ? old_path : "");
    ::setenv("PATH", new_path.c_str(), 1);

    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());

    const int64_t tid = 12;
    std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());

    for (int i = 0; i < 20; ++i) {
        std::string err;
        REQUIRE(mgr.write_to_workspace(
            tid, "f" + std::to_string(i) + ".txt", "x", err));
    }

    const std::string listing = mgr.list_workspace(tid);
    CHECK(listing.size() <= static_cast<size_t>(cfg.list_max_bytes + 80));
    CHECK(listing.find("... [truncated at") != std::string::npos);

    if (old_path) ::setenv("PATH", old_path, 1);
    else ::unsetenv("PATH");
    fs::remove_all(root);
}

TEST_CASE("sandbox list: entry cap trailer reports file count, not bytes") {
    const std::string root = make_temp_root("listentries");
    SandboxConfig cfg;
    cfg.image = "unused";
    cfg.workspaces_root = root + "/workspaces";
    cfg.runtime = "docker";
    cfg.idle_seconds = 0;
    // Both caps armed; entry cap must win and must not claim a byte limit.
    cfg.list_max_bytes = 10 * 1024 * 1024;
    cfg.list_max_files = 3;

    const std::string bin = root + "/bin";
    fs::create_directories(bin);
    const std::string stub = bin + "/docker";
    {
        std::ofstream f(stub);
        f << "#!/bin/sh\nexit 0\n";
    }
    ::chmod(stub.c_str(), 0755);
    const char* old_path = std::getenv("PATH");
    std::string new_path = bin + ":" + (old_path ? old_path : "");
    ::setenv("PATH", new_path.c_str(), 1);

    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());

    const int64_t tid = 13;
    std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());

    for (int i = 0; i < 8; ++i) {
        std::string err;
        REQUIRE(mgr.write_to_workspace(
            tid, "e" + std::to_string(i) + ".txt", "x", err));
    }

    const std::string listing = mgr.list_workspace(tid);
    CHECK(listing.find("... [truncated — listing entry cap reached]") !=
          std::string::npos);
    CHECK(listing.find("... [truncated at") == std::string::npos);

    if (old_path) ::setenv("PATH", old_path, 1);
    else ::unsetenv("PATH");
    fs::remove_all(root);
}

TEST_CASE("sandbox list: caps apply during walk, not after full collect") {
    // Regression for #202: listing must early-exit once list_max_files
    // is reached, even when many more files remain deeper in the tree.
    const std::string root = make_temp_root("listearly");
    SandboxConfig cfg;
    cfg.image = "unused";
    cfg.workspaces_root = root + "/workspaces";
    cfg.runtime = "docker";
    cfg.idle_seconds = 0;
    cfg.list_max_bytes = 10 * 1024 * 1024;
    cfg.list_max_files = 2;

    const std::string bin = root + "/bin";
    fs::create_directories(bin);
    const std::string stub = bin + "/docker";
    {
        std::ofstream f(stub);
        f << "#!/bin/sh\nexit 0\n";
    }
    ::chmod(stub.c_str(), 0755);
    const char* old_path = std::getenv("PATH");
    std::string new_path = bin + ":" + (old_path ? old_path : "");
    ::setenv("PATH", new_path.c_str(), 1);

    SandboxManager mgr(cfg);
    REQUIRE(mgr.usable());

    const int64_t tid = 14;
    std::string ws = mgr.ensure_workspace(tid);
    REQUIRE_FALSE(ws.empty());

    // Nested trees so a pre-cap full collect would still walk everything.
    for (int d = 0; d < 4; ++d) {
        for (int i = 0; i < 25; ++i) {
            std::string err;
            REQUIRE(mgr.write_to_workspace(
                tid,
                "d" + std::to_string(d) + "/f" + std::to_string(i) + ".txt",
                "x", err));
        }
    }

    const std::string listing = mgr.list_workspace(tid);
    CHECK(listing.find("... [truncated — listing entry cap reached]") !=
          std::string::npos);

    size_t file_lines = 0;
    for (size_t i = 0; i < listing.size(); ) {
        const size_t nl = listing.find('\n', i);
        if (nl == std::string::npos) break;
        const std::string line = listing.substr(i, nl - i);
        i = nl + 1;
        if (line.rfind("... [truncated", 0) == 0) continue;
        if (line.find(" bytes") != std::string::npos) ++file_lines;
    }
    CHECK(file_lines == static_cast<size_t>(cfg.list_max_files));

    if (old_path) ::setenv("PATH", old_path, 1);
    else ::unsetenv("PATH");
    fs::remove_all(root);
}
