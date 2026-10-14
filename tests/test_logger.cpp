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
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>

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

TEST_CASE("TUI stderr redirect keeps operational logs off the terminal") {
    StderrCapture cap;
    Logger::global().set_format(LogFormat::Human);

    char tmpl[] = "/tmp/arbiter_tui_log.XXXXXX";
    REQUIRE(mkdtemp(tmpl) != nullptr);
    const std::string dir = tmpl;
    const std::string log_path = dir + "/tui.log";

    {
        TuiStderrRedirect redir(dir);
        REQUIRE(redir.active());
        CHECK(redir.path() == log_path);
        std::fprintf(stderr,
                     "[info] openrouter_response requested_model=%s served_model=%s cost_usd=%s\n",
                     "anthropic/claude-haiku-4-5",
                     "anthropic/claude-haiku-4.5",
                     "0.001197");
        std::fflush(stderr);
        Logger::global().info("intent_route", {{"tier", "probe"}, {"source", "probe"}});
        Logger::global().info("intent_shadow", {{"backend", "probe"}});
        std::cerr << "[info] cerr-must-not-hit-terminal\n";
        std::cerr.flush();
    }

    const std::string leaked = cap.read();
    CHECK(leaked.find("openrouter_response") == std::string::npos);
    CHECK(leaked.find("intent_route") == std::string::npos);
    CHECK(leaked.find("intent_shadow") == std::string::npos);
    CHECK(leaked.find("cerr-must-not-hit-terminal") == std::string::npos);

    std::ifstream in(log_path);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string logged = ss.str();
    CHECK(contains(logged, "[info] openrouter_response requested_model=anthropic/claude-haiku-4-5"));
    CHECK(contains(logged, "served_model=anthropic/claude-haiku-4.5"));
    CHECK(contains(logged, "cost_usd=0.001197"));
    CHECK(contains(logged, "intent_route"));
    CHECK(contains(logged, "tier=probe"));
    CHECK(contains(logged, "intent_shadow"));
    CHECK(contains(logged, "cerr-must-not-hit-terminal"));

    Logger::global().info("after_restore", {{"ok", "1"}});
    CHECK(contains(cap.read(), "after_restore"));

    std::remove(log_path.c_str());
    ::rmdir(dir.c_str());
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
