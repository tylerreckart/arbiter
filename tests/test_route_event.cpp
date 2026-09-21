// tests/test_route_event.cpp — event routing for file-backed and
// preloaded (tenant) agent constitutions.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "event_routing.h"
#include "json.h"

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

TEST_CASE("route_event file scan returns empty when no match") {
    const std::string dir = make_temp_agents_dir();
    write_agent(dir, "facilities", R"({
        "name": "facilities",
        "model": "ollama/qwen",
        "goal": "watch sensors",
        "event_types": ["sensor.*"]
    })");
    CHECK(route_event(dir, "sensor.temp.high") == "facilities");
    CHECK(route_event(dir, "deploy.failed").empty());
    fs::remove_all(dir);
}

TEST_CASE("route_event skips agents without event_types") {
    const std::string dir = make_temp_agents_dir();
    write_agent(dir, "coder", R"({
        "name": "coder",
        "model": "ollama/qwen",
        "goal": "write code"
    })");
    CHECK(route_event(dir, "sensor.temp").empty());
    fs::remove_all(dir);
}

TEST_CASE("route_event uses filename stem when name is Title Case") {
    const std::string dir = make_temp_agents_dir();
    write_agent(dir, "scout", R"({
        "name": "Scout",
        "model": "ollama/qwen",
        "goal": "watch sensors",
        "event_types": ["sensor.*"]
    })");
    CHECK(route_event(dir, "sensor.temp.high") == "scout");
    fs::remove_all(dir);
}

TEST_CASE("file_backed_agent_def_json loads matching stem and stamps nothing") {
    const std::string dir = make_temp_agents_dir();
    write_agent(dir, "facilities", R"({
        "name": "facilities",
        "model": "ollama/qwen",
        "goal": "watch sensors",
        "event_types": ["sensor.*"]
    })");
    write_agent(dir, "coder", R"({
        "name": "coder",
        "model": "ollama/qwen",
        "goal": "write code"
    })");
    const std::string blob = file_backed_agent_def_json(dir, "facilities");
    REQUIRE_FALSE(blob.empty());
    auto j = json_parse(blob);
    REQUIRE(j);
    REQUIRE(j->is_object());
    CHECK(j->get_string("goal") == "watch sensors");
    CHECK(file_backed_agent_def_json(dir, "missing").empty());
    CHECK(file_backed_agent_def_json("/no/such/agents", "facilities").empty());
    fs::remove_all(dir);
}

TEST_CASE("file_backed_agent_def_json uses filename stem for Title Case names") {
    const std::string dir = make_temp_agents_dir();
    write_agent(dir, "scout", R"({
        "name": "Scout",
        "model": "ollama/qwen",
        "goal": "recon",
        "event_types": ["sensor.*"]
    })");
    CHECK(file_backed_agent_def_json(dir, "Scout").empty());
    const std::string blob = file_backed_agent_def_json(dir, "scout");
    REQUIRE_FALSE(blob.empty());
    CHECK(json_parse(blob)->get_string("goal") == "recon");
    fs::remove_all(dir);
}

TEST_CASE("event_ingest_agent_def_json prefers file blob when file routing won") {
    const std::string dir = make_temp_agents_dir();
    write_agent(dir, "facilities", R"({
        "name": "facilities",
        "model": "ollama/qwen",
        "goal": "file goal",
        "event_types": ["sensor.*"]
    })");
    const std::string tenant = R"({
        "name": "facilities",
        "model": "anthropic/claude",
        "goal": "tenant goal"
    })";
    const std::string blob = event_ingest_agent_def_json(
        "facilities", dir, tenant, /*routed_from_file=*/true);
    REQUIRE_FALSE(blob.empty());
    auto j = json_parse(blob);
    CHECK(j->get_string("id") == "facilities");
    CHECK(j->get_string("goal") == "file goal");
    fs::remove_all(dir);
}

TEST_CASE("event_ingest_agent_def_json prefers tenant blob unless file routed") {
    const std::string dir = make_temp_agents_dir();
    write_agent(dir, "facilities", R"({
        "name": "facilities",
        "model": "ollama/qwen",
        "goal": "file goal",
        "event_types": ["sensor.*"]
    })");
    const std::string tenant = R"({
        "name": "facilities",
        "model": "anthropic/claude",
        "goal": "tenant goal"
    })";
    const std::string blob = event_ingest_agent_def_json(
        "facilities", dir, tenant, /*routed_from_file=*/false);
    auto j = json_parse(blob);
    CHECK(j->get_string("id") == "facilities");
    CHECK(j->get_string("goal") == "tenant goal");
    fs::remove_all(dir);
}

TEST_CASE("event_ingest_agent_def_json falls back to file for explicit miss") {
    const std::string dir = make_temp_agents_dir();
    write_agent(dir, "facilities", R"({
        "name": "facilities",
        "model": "ollama/qwen",
        "goal": "file goal",
        "event_types": ["sensor.*"]
    })");
    const std::string blob = event_ingest_agent_def_json(
        "facilities", dir, /*tenant_def_json=*/"", /*routed_from_file=*/false);
    auto j = json_parse(blob);
    CHECK(j->get_string("id") == "facilities");
    CHECK(j->get_string("goal") == "file goal");
    CHECK(event_ingest_agent_def_json("index", dir, "", false).empty());
    CHECK(event_ingest_agent_def_json("ghost", dir, "", false).empty());
    fs::remove_all(dir);
}
