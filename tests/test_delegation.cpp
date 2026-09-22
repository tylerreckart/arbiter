// tests/test_delegation.cpp — Runtime spawn gates for constitution
// delegation policy.  No live LLM: denials happen before send_internal.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "orchestrator.h"
#include "constitution.h"

#include <string>

using namespace arbiter;

static Constitution specialist(const std::string& name) {
    Constitution c;
    c.name = name;
    c.role = "specialist";
    c.model = "anthropic/claude-sonnet-5";
    c.capabilities = {"/agent", "/parallel"};
    return c;
}

static void add_roster(Orchestrator& orch) {
    orch.create_agent("forge", specialist("forge"));
    orch.create_agent("scout", specialist("scout"));
    orch.create_agent("vera", specialist("vera"));
}

TEST_CASE("check_delegation_spawn: stock index allows catalog callees") {
    Orchestrator orch({});
    add_roster(orch);
    const auto& idx = orch.get_constitution("index");
    CHECK(idx.delegation.is_default());
    CHECK(orch.check_delegation_spawn("index", idx, "forge", 1).empty());
    CHECK(orch.check_delegation_spawn("index", idx, "forge", 2).empty());
}

TEST_CASE("/agent: forbidden callee (denied_callees forge)") {
    Orchestrator orch({});
    add_roster(orch);
    orch.get_agent("index").config_mut().delegation.denied_callees = {"forge"};

    std::string out = orch.execute_slash_command("/agent forge do the deploy", "index");
    CHECK(out.find("ERR:") != std::string::npos);
    CHECK(out.find("forge") != std::string::npos);
    CHECK(out.find("denied_callees") != std::string::npos);

    CHECK(orch.check_delegation_spawn(
        "index", orch.get_constitution("index"), "scout", 1).empty());
}

TEST_CASE("/agent: allowlist that omits forge") {
    Orchestrator orch({});
    add_roster(orch);
    orch.get_agent("index").config_mut().delegation.allowed_callees = {"scout", "vera"};

    std::string out = orch.execute_slash_command("/agent forge hello", "index");
    CHECK(out.find("ERR:") != std::string::npos);
    CHECK(out.find("allowed_callees") != std::string::npos);
}

TEST_CASE("/agent and /parallel: max_depth 0") {
    Orchestrator orch({});
    add_roster(orch);
    orch.get_agent("index").config_mut().delegation.max_depth = 0;

    std::string out = orch.execute_slash_command("/agent scout hi", "index");
    CHECK(out.find("ERR:") != std::string::npos);
    CHECK(out.find("max_depth 0") != std::string::npos);

    out = orch.execute_slash_command(
        "/parallel\n/agent scout a\n/agent vera b\n/endparallel", "index");
    CHECK(out.find("ERR:") != std::string::npos);
    CHECK(out.find("max_depth 0") != std::string::npos);
}

TEST_CASE("/parallel: forbidden callee") {
    Orchestrator orch({});
    add_roster(orch);
    orch.get_agent("index").config_mut().delegation.denied_callees = {"forge"};

    std::string out = orch.execute_slash_command(
        "/parallel\n/agent forge ship it\n/endparallel", "index");
    CHECK(out.find("ERR:") != std::string::npos);
    CHECK(out.find("denied_callees") != std::string::npos);
}

TEST_CASE("max_depth 1: depth-0 spawn ok, depth-2 spawn denied") {
    Orchestrator orch({});
    add_roster(orch);
    orch.get_agent("scout").config_mut().delegation.max_depth = 1;
    const auto& scout = orch.get_constitution("scout");

    CHECK(orch.check_delegation_spawn("scout", scout, "vera", 1).empty());
    std::string err = orch.check_delegation_spawn("scout", scout, "vera", 2);
    CHECK(err.find("ERR:") == 0);
    CHECK(err.find("max_depth 1") != std::string::npos);

    // Index (max_depth default 2) can still send work to scout at depth 1.
    CHECK(orch.check_delegation_spawn("index", orch.get_constitution("index"),
                                      "scout", 1).empty());
}

TEST_CASE("budget exceeded stops further spawn") {
    Orchestrator orch({});
    add_roster(orch);
    orch.get_agent("index").config_mut().delegation.max_subtree_tokens = 500;
    orch.record_delegation_spend("index", 500, 0);

    std::string out = orch.execute_slash_command("/agent scout hi", "index");
    CHECK(out.find("token budget exceeded") != std::string::npos);

    orch.get_agent("index").config_mut().delegation.max_subtree_tokens.reset();
    orch.get_agent("index").config_mut().delegation.max_subtree_usd = 0.01;
    orch.record_delegation_spend("index", 0, 0.01);
    out = orch.execute_slash_command("/agent scout hi", "index");
    CHECK(out.find("spend budget exceeded") != std::string::npos);
}

TEST_CASE("JIT run_ephemeral honours caller deny-forge and callee max_depth") {
    Orchestrator orch({});
    add_roster(orch);
    Constitution cfg = specialist("forge");

    orch.get_agent("index").config_mut().delegation.denied_callees = {"forge"};
    ApiResponse denied = orch.run_ephemeral("forge", cfg, "close the clause");
    CHECK_FALSE(denied.ok);
    CHECK(denied.error_type == "delegation_policy");
    CHECK(denied.error.find("denied_callees") != std::string::npos);

    orch.get_agent("index").config_mut().delegation = {};
    cfg.delegation.max_depth = 0;
    ApiResponse too_deep = orch.run_ephemeral("forge", cfg, "close the clause");
    CHECK_FALSE(too_deep.ok);
    CHECK(too_deep.error_type == "delegation_policy");
    CHECK(too_deep.error.find("cannot run at depth 1") != std::string::npos);
}

TEST_CASE("presence-style always_on constitution still has default delegation") {
    // Guard: adding delegation must not change presence defaults or imply
    // that presence consults the spawn gate.
    Constitution c = Constitution::from_json(R"({
        "name": "jules",
        "presence": "always_on"
    })");
    CHECK(c.presence.mode == "always_on");
    CHECK(c.delegation.is_default());
}
