// tests/test_intent.cpp — Unit tests for the pre-dispatch intent classifier.
// Pins cue matching, roster routing, LLM signal parse, fail-open, and
// apply_routing so format drift cannot silently misroute ingress.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "intent.h"

using namespace arbiter;

static std::vector<IntentRosterEntry> starter_roster() {
    return {
        {"scout", "research-analyst", "Research with depth", {"/search", "/fetch"}},
        {"vera", "code-reviewer", "Inspect real code", {"/exec"}},
        {"quill", "content-writer", "Produce polished writing", {"/write"}},
        {"forge", "infrastructure-engineer", "Build and operate infra", {"/exec"}},
        {"loom", "senior-frontend-engineer", "Ship working UI", {"/write"}},
        {"nexus", "senior-backend-engineer", "Design backend systems", {"/write"}},
        {"compass", "task-planner", "Decompose complex work", {"/write"}},
        {"beacon", "marketing-strategist", "Develop marketing strategy", {"/write"}},
        {"echo", "social-media-strategist", "Create platform-native content", {"/write"}},
    };
}

static IntentConfig hybrid_cfg() {
    IntentConfig c;
    c.mode = "hybrid";
    c.min_confidence = 0.8;
    c.apply_routing = true;
    return c;
}

TEST_CASE("cue: research maps uniquely to scout") {
    IntentInput in;
    in.text = "Look up primary sources for the 2024 RISC-V memory model";
    in.requested_agent = "index";
    in.roster = starter_roster();
    auto out = heuristic_classify(in);
    CHECK(out.kind == "research");
    CHECK(out.target_agent == "scout");
    CHECK(out.confidence >= 0.8);
    CHECK(out.source == "heuristic");
}

TEST_CASE("cue: code review maps to vera") {
    IntentInput in;
    in.text = "Please code review this PR before we merge";
    in.requested_agent = "index";
    in.roster = starter_roster();
    auto out = heuristic_classify(in);
    CHECK(out.kind == "review");
    CHECK(out.target_agent == "vera");
    CHECK(out.confidence >= 0.8);
}

TEST_CASE("cue: docker/k8s maps to forge") {
    IntentInput in;
    in.text = "Write a Dockerfile and a kubernetes deploy manifest";
    in.requested_agent = "index";
    in.roster = starter_roster();
    auto out = heuristic_classify(in);
    CHECK(out.kind == "ops");
    CHECK(out.target_agent == "forge");
}

TEST_CASE("cue: decompose / multi-step becomes plan/multi") {
    IntentInput in;
    in.text = "Break this down into a multi-step plan across research and backend";
    in.requested_agent = "index";
    in.roster = starter_roster();
    auto out = heuristic_classify(in);
    CHECK((out.kind == "plan" || out.kind == "multi"));
    CHECK(out.confidence < 0.8);
}

TEST_CASE("no cues → unknown, empty target, low confidence") {
    IntentInput in;
    in.text = "Hello, how is the weather in the garden?";
    in.requested_agent = "index";
    in.roster = starter_roster();
    auto out = heuristic_classify(in);
    CHECK(out.kind == "unknown");
    CHECK(out.target_agent.empty());
    CHECK(out.confidence < 0.8);
}

TEST_CASE("unique roster match without preferred id still routes via role") {
    IntentInput in;
    in.text = "Look up primary sources on SPARC v9";
    in.requested_agent = "index";
    in.roster = {{"scout", "research-analyst", "Find facts", {"/search"}}};
    auto out = heuristic_classify(in);
    CHECK(out.kind == "research");
    CHECK(out.target_agent == "scout");
    CHECK(out.confidence >= 0.8);
}

TEST_CASE("kind fires but agent missing from roster → no target, below threshold") {
    IntentInput in;
    in.text = "Look up primary sources on SPARC v9";
    in.requested_agent = "index";
    in.roster = {{"forge", "infrastructure-engineer", "ops", {"/exec"}}};
    auto out = heuristic_classify(in);
    CHECK(out.kind == "research");
    CHECK(out.target_agent.empty());
    CHECK(out.confidence < 0.8);
}

TEST_CASE("explicit specialist never reroutes") {
    IntentInput in;
    in.text = "Look up primary sources on SPARC v9";
    in.requested_agent = "forge";
    in.roster = starter_roster();
    auto out = resolve_intent(in, hybrid_cfg(), nullptr);
    CHECK(out.source == "explicit");
    CHECK(out.target_agent == "forge");
    CHECK_FALSE(intent_should_apply(hybrid_cfg(), out, "forge"));
}

TEST_CASE("apply_routing false does not apply even when confident") {
    IntentInput in;
    in.text = "Look up primary sources on SPARC v9";
    in.requested_agent = "index";
    in.roster = starter_roster();
    auto cfg = hybrid_cfg();
    cfg.apply_routing = false;
    auto out = resolve_intent(in, cfg, nullptr);
    CHECK(out.target_agent == "scout");
    CHECK(out.confidence >= 0.8);
    CHECK_FALSE(intent_should_apply(cfg, out, "index"));
}

TEST_CASE("mode off → source none, no target on index") {
    IntentInput in;
    in.text = "Look up primary sources on SPARC v9";
    in.requested_agent = "index";
    in.roster = starter_roster();
    IntentConfig cfg;  // mode off
    auto out = resolve_intent(in, cfg, nullptr);
    CHECK(out.source == "none");
    CHECK(out.target_agent.empty());
    CHECK(out.kind == "unknown");
}

TEST_CASE("hybrid without llm fn degrades to heuristic") {
    IntentInput in;
    in.text = "Hello there";
    in.requested_agent = "index";
    in.roster = starter_roster();
    auto out = resolve_intent(in, hybrid_cfg(), nullptr);
    CHECK(out.source == "heuristic");
    CHECK(out.target_agent.empty());
    CHECK_FALSE(out.llm_used);
}

TEST_CASE("hybrid unconfident calls llm and parses signal") {
    IntentInput in;
    in.text = "Hello there, please help";
    in.requested_agent = "index";
    in.roster = starter_roster();
    bool called = false;
    auto llm = [&](const std::string& prompt) {
        called = true;
        CHECK(prompt.find("[REQUEST]") != std::string::npos);
        CHECK(prompt.find("research") != std::string::npos);
        return std::string(
            "<intent>\n"
            "<kind>research</kind>\n"
            "<confidence>0.91</confidence>\n"
            "<agent>scout</agent>\n"
            "<brief>User wants a factual lookup.</brief>\n"
            "</intent>");
    };
    auto out = resolve_intent(in, hybrid_cfg(), llm);
    CHECK(called);
    CHECK(out.llm_used);
    CHECK(out.kind == "research");
    CHECK(out.target_agent == "scout");
    CHECK(out.confidence == doctest::Approx(0.91));
    CHECK(out.brief.find("factual") != std::string::npos);
    CHECK(intent_should_apply(hybrid_cfg(), out, "index"));
}

TEST_CASE("malformed llm reply fail-open: no target") {
    IntentInput in;
    in.text = "Hello there";
    in.requested_agent = "index";
    in.roster = starter_roster();
    auto llm = [&](const std::string&) {
        return std::string("sure, I think this is research related");
    };
    auto out = resolve_intent(in, hybrid_cfg(), llm);
    CHECK(out.llm_used);
    CHECK(out.malformed);
    CHECK(out.target_agent.empty());
    CHECK_FALSE(intent_should_apply(hybrid_cfg(), out, "index"));
}

TEST_CASE("empty llm reply fail-open keeps heuristic") {
    IntentInput in;
    in.text = "Hello there";
    in.requested_agent = "index";
    in.roster = starter_roster();
    auto llm = [&](const std::string&) { return std::string{}; };
    auto out = resolve_intent(in, hybrid_cfg(), llm);
    CHECK(out.llm_used);
    CHECK(out.target_agent.empty());
    CHECK(out.kind == "unknown");
}

TEST_CASE("unknown agent id in llm reply is dropped") {
    IntentInput in;
    in.text = "Hello there";
    in.requested_agent = "index";
    in.roster = starter_roster();
    auto llm = [&](const std::string&) {
        return std::string(
            "<intent><kind>research</kind><confidence>0.95</confidence>"
            "<agent>not-a-real-agent</agent><brief>x</brief></intent>");
    };
    auto out = resolve_intent(in, hybrid_cfg(), llm);
    CHECK(out.kind == "research");
    CHECK(out.target_agent.empty());
}

TEST_CASE("parse: todo and phase seeds") {
    auto out = parse_intent_signal(
        "<intent>\n"
        "<kind>multi</kind>\n"
        "<confidence>0.88</confidence>\n"
        "<agent>compass</agent>\n"
        "<brief>Decompose the launch.</brief>\n"
        "<todo>Gather market comps</todo>\n"
        "<todo>Draft API sketch</todo>\n"
        "<phase agent=\"scout\" name=\"survey\">Find primary sources</phase>\n"
        "<phase agent=\"nexus\" name=\"api\">Sketch the schema</phase>\n"
        "</intent>");
    CHECK(out.malformed == false);
    CHECK(out.kind == "multi");
    CHECK(out.target_agent == "compass");
    CHECK(out.todo_seeds.size() == 2);
    CHECK(out.todo_seeds[0].title == "Gather market comps");
    CHECK(out.todo_seeds[1].title == "Draft API sketch");
    REQUIRE(out.plan_seeds.size() == 2);
    CHECK(out.plan_seeds[0].agent == "scout");
    CHECK(out.plan_seeds[0].name == "survey");
    CHECK(out.plan_seeds[0].task == "Find primary sources");
    CHECK(out.plan_seeds[1].agent == "nexus");
}

TEST_CASE("parse: caps todo and phase seed counts") {
    std::string body =
        "<intent><kind>multi</kind><confidence>0.9</confidence>"
        "<agent>compass</agent><brief>x</brief>";
    for (int i = 0; i < 40; ++i) {
        body += "<todo>item " + std::to_string(i) + "</todo>";
    }
    for (int i = 0; i < 20; ++i) {
        body += "<phase agent=\"scout\" name=\"p" + std::to_string(i) +
                "\">task</phase>";
    }
    body += "</intent>";
    auto out = parse_intent_signal(body);
    CHECK(out.todo_seeds.size() == 32);
    CHECK(out.plan_seeds.size() == 16);
}

TEST_CASE("parse: invalid kind is malformed unknown") {
    auto out = parse_intent_signal(
        "<intent><kind>banana</kind><confidence>0.9</confidence>"
        "<agent></agent></intent>");
    CHECK(out.malformed);
    CHECK(out.kind == "unknown");
}

TEST_CASE("event source hint overrides heuristic source label") {
    IntentInput in;
    in.text = "Look up primary sources on SPARC v9";
    in.requested_agent = "index";
    in.roster = starter_roster();
    in.source_hint = "event";
    auto out = resolve_intent(in, hybrid_cfg(), nullptr);
    CHECK(out.source == "event");
    CHECK(out.target_agent == "scout");
}

TEST_CASE("preamble: index hint and specialist GOAL") {
    Intent in;
    in.kind = "research";
    in.confidence = 0.91;
    in.source = "heuristic";
    in.target_agent = "scout";
    in.brief = "Find primary sources.";
    auto idx = format_intent_preamble(in, false);
    CHECK(idx.find("[INTENT] kind=research") != std::string::npos);
    CHECK(idx.find("source=heuristic") != std::string::npos);
    CHECK(idx.find("GOAL:") == std::string::npos);
    auto spec = format_intent_preamble(in, true);
    CHECK(spec.find("GOAL: Find primary sources.") != std::string::npos);
}

TEST_CASE("intent_should_apply requires index request + confidence + target") {
    Intent in;
    in.kind = "research";
    in.confidence = 0.91;
    in.target_agent = "scout";
    auto cfg = hybrid_cfg();
    CHECK(intent_should_apply(cfg, in, "index"));
    CHECK(intent_should_apply(cfg, in, ""));
    CHECK_FALSE(intent_should_apply(cfg, in, "scout"));
    in.confidence = 0.5;
    CHECK_FALSE(intent_should_apply(cfg, in, "index"));
}

TEST_CASE("intent_should_apply false on continuation (not fresh ingress)") {
    Intent in;
    in.kind = "research";
    in.confidence = 0.91;
    in.target_agent = "scout";
    CHECK_FALSE(intent_should_apply(hybrid_cfg(), in, "index", false));
    CHECK(intent_should_apply(hybrid_cfg(), in, "index", true));
}

TEST_CASE("cues: bare deploy/react/sql/cite do not false-positive") {
    IntentInput in;
    in.requested_agent = "index";
    in.roster = starter_roster();

    in.text = "Please deploy this feature when you get a chance";
    auto ops = heuristic_classify(in);
    CHECK(ops.kind != "ops");

    in.text = "Can you react to this feedback from the team?";
    auto fe = heuristic_classify(in);
    CHECK(fe.kind != "frontend");

    in.text = "What's the sql for last quarter's report?";
    auto be = heuristic_classify(in);
    CHECK(be.kind != "backend");

    in.text = "Please cite your sources in the summary";
    auto rs = heuristic_classify(in);
    CHECK(rs.kind != "research");
}

TEST_CASE("standalone classify defaults to heuristic, keeps master thresholds") {
    IntentConfig master;
    master.mode = "hybrid";
    master.min_confidence = 0.9;
    master.apply_routing = true;
    master.model = "advisor-model";
    auto cfg = standalone_intent_config(master);
    CHECK(cfg.mode == "heuristic");
    CHECK(cfg.min_confidence == doctest::Approx(0.9));
    CHECK(cfg.apply_routing);
    CHECK(cfg.model == "advisor-model");
}

TEST_CASE("standalone heuristic does not invoke llm on unconfident text") {
    IntentInput in;
    in.text = "Hello there, please help";
    in.requested_agent = "index";
    in.roster = starter_roster();
    bool called = false;
    auto llm = [&](const std::string&) {
        called = true;
        return std::string{};
    };
    IntentConfig master;
    master.mode = "hybrid";
    auto cfg = standalone_intent_config(master);
    auto out = resolve_intent(in, cfg, llm);
    CHECK_FALSE(called);
    CHECK_FALSE(out.llm_used);
    CHECK(out.source == "heuristic");
}
