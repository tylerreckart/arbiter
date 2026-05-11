// tests/test_logger.cpp — Smoke tests for the format switch.  The
// logger writes to stderr by design; we redirect stderr into a temp
// file for the duration of each check.  Verifies the human / json
// modes produce parseable, level-tagged lines and that escaping
// covers the troublesome bytes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "logger.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace arbiter;

namespace {

struct StderrCapture {
    std::string                path;
    int                        saved_fd = -1;
    StderrCapture() {
        char tmpl[] = "/tmp/arbiter_log_test.XXXXXX";
        int fd = mkstemp(tmpl);
        REQUIRE(fd >= 0);
        path = tmpl;
        std::fflush(stderr);
        saved_fd = dup(fileno(stderr));
        dup2(fd, fileno(stderr));
        close(fd);
    }
    ~StderrCapture() {
        std::fflush(stderr);
        if (saved_fd >= 0) {
            dup2(saved_fd, fileno(stderr));
            close(saved_fd);
        }
        std::remove(path.c_str());
    }
    std::string read() {
        std::fflush(stderr);
        std::ifstream f(path);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
};

bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("human format includes level and event") {
    StderrCapture cap;
    Logger::global().set_format(LogFormat::Human);
    Logger::global().info("startup", {{"port", "8080"}});
    auto out = cap.read();
    CHECK(contains(out, "[info]"));
    CHECK(contains(out, "startup"));
    CHECK(contains(out, "port=8080"));
}

TEST_CASE("json format is one self-contained line per event") {
    StderrCapture cap;
    Logger::global().set_format(LogFormat::Json);
    Logger::global().warn("sandbox_disabled",
        {{"reason", "docker not found"}});
    auto out = cap.read();
    CHECK(out.find("\n") != std::string::npos);
    CHECK(contains(out, "\"level\":\"warn\""));
    CHECK(contains(out, "\"event\":\"sandbox_disabled\""));
    CHECK(contains(out, "\"reason\":\"docker not found\""));
    CHECK(contains(out, "\"ts\":\""));
}

TEST_CASE("json escapes troublesome bytes") {
    StderrCapture cap;
    Logger::global().set_format(LogFormat::Json);
    Logger::global().error("bad", {{"field", "line\nwith\"quotes\\and"}});
    auto out = cap.read();
    CHECK(contains(out, "\"field\":\"line\\nwith\\\"quotes\\\\and\""));
}

TEST_CASE("each call writes exactly one line") {
    StderrCapture cap;
    Logger::global().set_format(LogFormat::Json);
    Logger::global().info("a");
    Logger::global().info("b");
    Logger::global().info("c");
    auto out = cap.read();
    int newlines = 0;
    for (char c : out) if (c == '\n') ++newlines;
    CHECK(newlines == 3);
}
