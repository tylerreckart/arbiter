// tests/test_constitution.cpp — Regression coverage for the prompt
// composer.  The composer turns an agent's `capabilities` vector into a
// system prompt by emitting only the inventory + rules for enabled
// bundles.  Empty capabilities preserves the legacy monolithic surface
// (back-compat for the master orchestrator).
//
// These tests pin the bundle-resolution semantics and check that
// restricted agents actually get a smaller prompt than master.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "constitution.h"

using namespace arbiter;

// Helper: construct a minimal Constitution and run the composer.  Anchored
// to claude-sonnet-4-6 so we hit the arbiter_prompt path, not the weak-
// executor profile (which has its own structure and is not the subject of
// this refactor).
static Constitution make_agent(std::vector<std::string> caps = {}) {
    Constitution c;
    c.name = "test";
    c.role = "tester";
    c.brevity = Brevity::Full;
    c.model = "claude-sonnet-4-6";
    c.capabilities = std::move(caps);
    return c;
}

TEST_CASE("empty capabilities yields the legacy surface (master back-compat)") {
    auto c = make_agent();
    auto prompt = c.build_system_prompt();

    // All legacy section headings should still appear so the master
    // orchestrator's behaviour stays unchanged after the bundle split.
    CHECK(prompt.find("VOICE:")                          != std::string::npos);
    CHECK(prompt.find("COMPRESSION RULES:")              != std::string::npos);
    CHECK(prompt.find("EXCEPTIONS")                      != std::string::npos);
    CHECK(prompt.find("CAPABILITIES:")                   != std::string::npos);
    CHECK(prompt.find("Available commands:")             != std::string::npos);
    CHECK(prompt.find("COMMAND RULES:")                  != std::string::npos);
    CHECK(prompt.find("REASONING:")                      != std::string::npos);
    CHECK(prompt.find("DELEGATION-TURN OUTPUT DISCIPLINE:") != std::string::npos);
    CHECK(prompt.find("INTER-AGENT RESPONSE FORMAT:")    != std::string::npos);

    // All default-bundle inventory rows should appear.
    CHECK(prompt.find("/search ")    != std::string::npos);
    CHECK(prompt.find("/fetch ")     != std::string::npos);
    CHECK(prompt.find("/browse ")    != std::string::npos);
    CHECK(prompt.find("/exec ")      != std::string::npos);
    CHECK(prompt.find("/agent ")     != std::string::npos);
    CHECK(prompt.find("/parallel ")  != std::string::npos);
    CHECK(prompt.find("/pane ")      != std::string::npos);
    CHECK(prompt.find("/write ")     != std::string::npos);
    CHECK(prompt.find("/read ")      != std::string::npos);
    CHECK(prompt.find("/list")       != std::string::npos);
    CHECK(prompt.find("/mem write")  != std::string::npos);
    CHECK(prompt.find("/mem search") != std::string::npos);
    CHECK(prompt.find("/help ")      != std::string::npos);

    // /mcp is intentionally OUT of the default set (it was never in the
    // legacy prompt).  Agents that want /mcp must list it in capabilities.
    CHECK(prompt.find("/mcp tools") == std::string::npos);
}

TEST_CASE("restricted capabilities produces a smaller prompt") {
    auto full     = make_agent();
    auto narrow   = make_agent({"/exec", "/write"});
    auto pfull    = full.build_system_prompt();
    auto pnarrow  = narrow.build_system_prompt();

    CHECK(pnarrow.size() < pfull.size());

    // Only the requested bundles' inventory should appear.
    CHECK(pnarrow.find("/exec ")     != std::string::npos);
    CHECK(pnarrow.find("/write ")    != std::string::npos);

    // Bundles not listed should be absent from the inventory.  Anchor on
    // the inventory's right-hand description text — the mere token "/agent"
    // also appears in the always-on INTER-AGENT RESPONSE FORMAT section
    // ("When invoked via /agent..."), which is correct and unrelated.
    CHECK(pnarrow.find("/search <query>")  == std::string::npos);
    CHECK(pnarrow.find("sub-agent inline") == std::string::npos);
    CHECK(pnarrow.find("/mem write <text>")== std::string::npos);
    CHECK(pnarrow.find("read artifact")    == std::string::npos);

    // Delegation discipline only emits when the delegation bundle is on —
    // there's no point teaching it to an agent that can't /agent.
    CHECK(pnarrow.find("DELEGATION-TURN OUTPUT DISCIPLINE:") == std::string::npos);

    // Always-on sections still appear.
    CHECK(pnarrow.find("VOICE:")                       != std::string::npos);
    CHECK(pnarrow.find("REASONING:")                   != std::string::npos);
    CHECK(pnarrow.find("INTER-AGENT RESPONSE FORMAT:") != std::string::npos);
}

TEST_CASE("delegation bundle pulls in delegation discipline") {
    auto c = make_agent({"/agent", "/parallel"});
    auto p = c.build_system_prompt();

    CHECK(p.find("/agent ")    != std::string::npos);
    CHECK(p.find("/parallel ") != std::string::npos);
    CHECK(p.find("DELEGATION-TURN OUTPUT DISCIPLINE:") != std::string::npos);
}

TEST_CASE("artifact-pairing rule emits only when write + mem are both enabled") {
    auto write_only = make_agent({"/write"});
    auto mem_only   = make_agent({"/mem"});
    auto both       = make_agent({"/write", "/mem"});

    const char* needle = "For files the user may want to refine later";
    CHECK(write_only.build_system_prompt().find(needle) == std::string::npos);
    CHECK(mem_only  .build_system_prompt().find(needle) == std::string::npos);
    CHECK(both      .build_system_prompt().find(needle) != std::string::npos);
}

TEST_CASE("mcp must be opt-in via /mcp in capabilities") {
    auto without = make_agent({"/exec"});
    auto with    = make_agent({"/exec", "/mcp"});

    CHECK(without.build_system_prompt().find("/mcp tools") == std::string::npos);
    CHECK(with   .build_system_prompt().find("/mcp tools") != std::string::npos);
}

TEST_CASE("starter-agent capability sets produce measurably smaller prompts") {
    // Anchored on the actual capability sets in src/starters.cpp so any
    // future drift between starters and the bundle splitter shows up here.
    auto master   = make_agent();   // empty caps → all default bundles
    auto devops   = make_agent({"/exec", "/write"});
    auto research = make_agent({"/search", "/fetch", "/browse", "/mem", "/agent"});
    auto reviewer = make_agent({"/exec", "/write", "/agent"});

    auto p_master   = master  .build_system_prompt();
    auto p_devops   = devops  .build_system_prompt();
    auto p_research = research.build_system_prompt();
    auto p_reviewer = reviewer.build_system_prompt();

    // Each restricted profile should be smaller than the master.
    CHECK(p_devops  .size() < p_master.size());
    CHECK(p_research.size() < p_master.size());
    CHECK(p_reviewer.size() < p_master.size());

    // Devops doesn't research the web → the largest savings come from
    // dropping the web + delegation + mem + read bundles.  Sanity check
    // that the savings are at least 25% (the unit is bytes, but the ratio
    // tracks token count linearly enough for a regression guard).
    CHECK((double)p_devops.size() / (double)p_master.size() < 0.75);

    // Print sizes once so a maintainer running the suite by hand sees the
    // actual reduction.  doctest's MESSAGE goes to stderr without failing.
    MESSAGE("master   prompt: " << p_master.size()   << " bytes");
    MESSAGE("research prompt: " << p_research.size() << " bytes");
    MESSAGE("devops   prompt: " << p_devops.size()   << " bytes");
    MESSAGE("reviewer prompt: " << p_reviewer.size() << " bytes");
}

// Advisor parsing — covers the three valid JSON shapes plus the legacy/object
// precedence rule.  Pin the resolution so the bug where "advisor": "<model>"
// was silently dropped (because the parser only read "advisor_model") cannot
// regress.
TEST_CASE("advisor: legacy advisor_model populates AdvisorConfig in consult mode") {
    std::string js = R"({
        "name": "research",
        "model": "claude-haiku-4-5",
        "advisor_model": "claude-opus-4-6"
    })";
    auto c = Constitution::from_json(js);
    CHECK(c.advisor_model == "claude-opus-4-6");
    CHECK(c.advisor.model == "claude-opus-4-6");
    CHECK(c.advisor.mode  == "consult");
    CHECK(c.advisor.max_redirects == 2);
    CHECK(c.advisor.malformed_halts == true);
}

TEST_CASE("advisor: string shorthand parses as consult-mode object") {
    // This is the bug fix: backend.json / frontend.json / devops.json used
    // "advisor": "<model>" and the parser silently dropped it.  After the
    // fix, the string form maps to {model: <s>, mode: "consult"} and the
    // legacy advisor_model field is mirrored too so /advise still works.
    std::string js = R"({
        "name": "backend",
        "model": "claude-sonnet-4-6",
        "advisor": "claude-opus-4-7"
    })";
    auto c = Constitution::from_json(js);
    CHECK(c.advisor.model == "claude-opus-4-7");
    CHECK(c.advisor.mode  == "consult");
    CHECK(c.advisor_model == "claude-opus-4-7");  // mirrored for legacy /advise
}

TEST_CASE("advisor: object form with mode=gate populates all fields") {
    std::string js = R"({
        "name": "frontend",
        "model": "claude-sonnet-4-6",
        "advisor": {
            "model": "claude-opus-4-7",
            "mode": "gate",
            "max_redirects": 1,
            "malformed_halts": false
        }
    })";
    auto c = Constitution::from_json(js);
    CHECK(c.advisor.model == "claude-opus-4-7");
    CHECK(c.advisor.mode  == "gate");
    CHECK(c.advisor.max_redirects == 1);
    CHECK(c.advisor.malformed_halts == false);
}

TEST_CASE("advisor: object form wins when both legacy and object are present") {
    std::string js = R"({
        "name": "research",
        "model": "claude-haiku-4-5",
        "advisor_model": "claude-opus-4-5",
        "advisor": { "model": "claude-opus-4-7", "mode": "gate" }
    })";
    auto c = Constitution::from_json(js);
    CHECK(c.advisor.model == "claude-opus-4-7");  // object wins
    CHECK(c.advisor.mode  == "gate");
}

TEST_CASE("advisor: absent yields disabled config") {
    std::string js = R"({
        "name": "marketer",
        "model": "claude-sonnet-4-6"
    })";
    auto c = Constitution::from_json(js);
    CHECK(c.advisor.model.empty());
    CHECK(c.advisor.mode  == "consult");  // struct default
    CHECK(c.advisor_model.empty());
}

TEST_CASE("/mem variants all map to the mem bundle") {
    // Different agents historically declared "/mem", "/mem shared", "/mem add",
    // etc.  All should resolve to the same bundle so the prompt stays
    // consistent regardless of which sub-form an agent_def used.
    auto a = make_agent({"/mem"});
    auto b = make_agent({"/mem shared"});
    auto c = make_agent({"/mem add"});

    auto pa = a.build_system_prompt();
    auto pb = b.build_system_prompt();
    auto pc = c.build_system_prompt();

    for (const auto& p : {pa, pb, pc}) {
        CHECK(p.find("/mem write")  != std::string::npos);
        CHECK(p.find("/mem search") != std::string::npos);
        CHECK(p.find("/mem add ")   != std::string::npos);
    }
}
