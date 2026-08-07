// tests/test_route_event.cpp — event routing for file-backed and
// preloaded (tenant) agent constitutions.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "event_routing.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdlib.h>
#include <vector>

namespace fs = std::filesystem;
using namespace arbiter;

namespace {

std::string make_temp_agents_dir() {
    auto base = fs::temp_directory_path() / "arbiter-route-XXXXXX";
    std::string tmpl = base.string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* dir = ::mkdtemp(buf.data());
    REQUIRE(dir != nullptr);
    return std::string(dir);
}

void write_agent(const std::string& dir, const std::string& stem,
                 const std::string& json) {
    std::ofstream f(dir + "/" + stem + ".json");
    f << json;
    REQUIRE(f.good());
}

} // namespace

TEST_CASE("route_event list match returns first hit") {
    std::vector<std::pair<std::string, std::vector<std::string>>> agents = {
        {"alpha", {"sensor.temp.*"}},
        {"beta",  {"sensor.*", "facility.alert.*"}},
    };
    CHECK(route_event(agents, "sensor.temp.high") == "alpha");
    CHECK(route_event(agents, "sensor.humidity") == "beta");
    CHECK(route_event(agents, "facility.alert.fire") == "beta");
    CHECK(route_event(agents, "unrelated.event").empty());
}

TEST_CASE("route_event file scan falls back to index") {
    const std::string dir = make_temp_agents_dir();
    write_agent(dir, "facilities", R"({
        "name": "facilities",
        "model": "ollama/qwen",
        "goal": "watch sensors",
        "event_types": ["sensor.*"]
    })");
    CHECK(route_event(dir, "sensor.temp.high") == "facilities");
    CHECK(route_event(dir, "deploy.failed") == "index");
    fs::remove_all(dir);
}

TEST_CASE("route_event skips agents without event_types") {
    const std::string dir = make_temp_agents_dir();
    write_agent(dir, "coder", R"({
        "name": "coder",
        "model": "ollama/qwen",
        "goal": "write code"
    })");
    CHECK(route_event(dir, "sensor.temp") == "index");
    fs::remove_all(dir);
}
