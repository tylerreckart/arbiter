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
