// tests/test_intent_route.cpp — Tiered live router. No network.
// Luna is a mock. The generative <intent> classifier is not a parameter
// of route_intent_live, so these cases cannot call it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "intent_route.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

using namespace arbiter;

namespace {

std::vector<IntentRosterEntry> roster() {
    return {
        {"scout", "research-analyst", "Research with depth", {}},
        {"vera", "code-reviewer", "Inspect real code", {}},
        {"index", "master", "Route the request", {}},
    };
}

IntentConfig hybrid() {
    IntentConfig c;
    c.mode = "hybrid";
    c.min_confidence = 0.8;
    c.apply_routing = true;
    return c;
}

IntentRouteConfig route_cfg() {
    IntentRouteConfig c = intent_route_defaults();
    c.timeout_ms = 400;
    c.luna_min_confidence = 0.55;
    c.cache_ttl_ms = 60000;
    c.cache_entries = 8;
    c.default_agent = "index";
    return c;
}

IntentInput input(const std::string& text) {
    IntentInput in;
    in.text = text;
    in.requested_agent = "index";
    in.roster = roster();
    return in;
}

DecisionResult joint_result(double scout_cheap, double scout_strong,
                            double vera_cheap, double vera_strong) {
    DecisionResult r;
    r.ok = true;
    r.choice = "scout|strong";
    r.distribution = {
        {"scout|cheap", scout_cheap},
        {"scout|strong", scout_strong},
        {"vera|cheap", vera_cheap},
        {"vera|strong", vera_strong},
    };
    r.latency_ms = 12;
    return r;
}

bool has_both_bands(const DecisionRequest& req, const std::string& agent) {
    bool cheap = false;
    bool strong = false;
    for (const auto& opt : req.options) {
        if (opt.id == agent + "|cheap") cheap = true;
        if (opt.id == agent + "|strong") strong = true;
        if (opt.description.find("hello") != std::string::npos) return false;
    }
    return cheap && strong;
}

struct EnvGuard {
    std::string name;
    bool had = false;
    std::string prev;
    EnvGuard(const char* n, const char* value) : name(n) {
        if (const char* cur = std::getenv(n)) {
            had = true;
            prev = cur;
        }
        setenv(n, value, 1);
    }
    ~EnvGuard() {
        if (had) setenv(name.c_str(), prev.c_str(), 1);
        else unsetenv(name.c_str());
    }
};

} // namespace

TEST_CASE("tier 1: a strong heuristic does not call luna") {
    IntentRouteCache cache;
    int calls = 0;
    int loads = 0;
    auto out = route_intent_live(
        input("Look up primary sources for the 2024 RISC-V memory model"),
        hybrid(), route_cfg(), cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            ++calls;
            return DecisionResult{};
        },
        [&](const std::string&) { ++loads; });
    CHECK(calls == 0);
    CHECK(loads == 0);
    CHECK_FALSE(out.used_luna);
    CHECK(out.timings.tier == "heuristic");
    CHECK_FALSE(out.timings.cache_hit);
    CHECK(out.timings.luna_ms == 0);
    CHECK(out.intent.target_agent == "scout");
    CHECK(out.intent.source == "heuristic");
    CHECK(out.intent.confidence >= 0.8);
    CHECK(intent_route_should_apply(hybrid(), out.intent, "index"));
}

TEST_CASE("tier 2: one luna call carries agent and difficulty") {
    IntentRouteCache cache;
    int calls = 0;
    DecisionRequest seen;
    auto out = route_intent_live(
        input("hello there"), hybrid(), route_cfg(), cache,
        [&](const DecisionRequest& req, const std::atomic<bool>&) {
            ++calls;
            seen = req;
            return joint_result(0.05, 0.90, 0.03, 0.02);
        });
    CHECK(calls == 1);
    CHECK(out.used_luna);
    CHECK(has_both_bands(seen, "scout"));
    CHECK(has_both_bands(seen, "vera"));
    CHECK(seen.state == "hello there");
    CHECK(seen.question.find("cheap") != std::string::npos);
    CHECK(out.timings.tier == "luna");
    CHECK(out.intent.source == "luna");
    CHECK(out.intent.target_agent == "scout");
    CHECK(out.intent.confidence == doctest::Approx(0.9));
    CHECK(out.timings.difficulty_known);
    CHECK(out.timings.difficulty == doctest::Approx(0.92));
    CHECK(out.timings.model_tier == "strong");
    CHECK(out.timings.choose_model);
    CHECK(intent_route_should_apply(hybrid(), out.intent, "index"));
}

TEST_CASE("low confidence still routes to the luna guess") {
    IntentRouteCache cache;
    auto out = route_intent_live(
        input("hello there"), hybrid(), route_cfg(), cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            return joint_result(0.40, 0.20, 0.30, 0.10);
        });
    CHECK(out.timings.tier == "luna_guess");
    CHECK(out.intent.source == "luna_guess");
    CHECK(out.intent.target_agent == "scout");
    CHECK(out.intent.confidence == doctest::Approx(0.2));
    CHECK(out.intent.confidence < route_cfg().luna_min_confidence);
    CHECK(intent_route_should_apply(hybrid(), out.intent, "index"));
    CHECK(out.timings.model_tier == "cheap");
}

TEST_CASE("one observed logprob does not become a confident route") {
    IntentRouteCache cache;
    auto out = route_intent_live(
        input("hello there"), hybrid(), route_cfg(), cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            TokenLogprobs tok;
            tok.token = "A";
            tok.has_logprob = true;
            tok.logprob = std::log(0.42);
            tok.top_logprobs = {{"A", std::log(0.42)}};
            std::vector<DecisionOption> opts(4);
            opts[0].id = "scout|cheap";
            opts[0].label = "A";
            opts[1].id = "scout|strong";
            opts[1].label = "B";
            opts[2].id = "vera|cheap";
            opts[2].label = "C";
            opts[3].id = "vera|strong";
            opts[3].label = "D";
            return classify_logprobs(opts, {tok});
        });
    CHECK(out.timings.tier == "luna_guess");
    CHECK(out.intent.source == "luna_guess");
    CHECK(out.intent.target_agent == "scout");
    CHECK(out.intent.confidence == doctest::Approx(decision_confidence(0.42, 2)));
    CHECK(out.intent.confidence < route_cfg().luna_min_confidence);
    CHECK_FALSE(out.timings.difficulty_known);
}

TEST_CASE("luna error falls back and is not cached") {
    IntentRouteCache cache;
    int calls = 0;
    auto fail = [&](const DecisionRequest&, const std::atomic<bool>&) {
        ++calls;
        DecisionResult r;
        r.ok = false;
        r.error = "provider rejected logprobs";
        return r;
    };
    auto cfg = route_cfg();
    auto first = route_intent_live(input("hello there"), hybrid(), cfg, cache, fail);
    auto second = route_intent_live(input("hello there"), hybrid(), cfg, cache, fail);
    CHECK(calls == 2);
    CHECK(first.timings.tier == "fallback");
    CHECK(first.intent.source == "fallback");
    CHECK(first.intent.target_agent.empty());
    CHECK(first.timings.error == "provider rejected logprobs");
    CHECK_FALSE(intent_route_should_apply(hybrid(), first.intent, "index"));
    CHECK(second.timings.tier == "fallback");
    CHECK_FALSE(second.timings.cache_hit);
}

TEST_CASE("timeout cancels luna and does not block on it") {
    IntentRouteCache cache;
    std::atomic<bool> saw_cancel{false};
    auto cfg = route_cfg();
    cfg.timeout_ms = 30;
    auto out = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>& cancel) {
            while (!cancel.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            saw_cancel.store(true, std::memory_order_release);
            DecisionResult r;
            r.ok = false;
            r.error = "cancelled";
            return r;
        });
    CHECK(out.timings.tier == "fallback");
    CHECK(out.used_luna);
    CHECK(out.intent.target_agent.empty());
    CHECK(out.timings.total_ms < 500);
    for (int i = 0; i < 50 && !saw_cancel.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(saw_cancel.load());
}

TEST_CASE("cache hit skips luna and ignores whitespace") {
    IntentRouteCache cache;
    int calls = 0;
    auto luna = [&](const DecisionRequest&, const std::atomic<bool>&) {
        ++calls;
        return joint_result(0.05, 0.90, 0.03, 0.02);
    };
    auto cfg = route_cfg();
    auto first = route_intent_live(input("hello there"), hybrid(), cfg, cache, luna);
    auto second = route_intent_live(input("Hello   There"), hybrid(), cfg, cache, luna);
    CHECK(calls == 1);
    CHECK(first.timings.tier == "luna");
    CHECK_FALSE(first.timings.cache_hit);
    CHECK(second.timings.tier == "cache");
    CHECK(second.timings.cache_hit);
    CHECK(second.timings.luna_ms == 0);
    CHECK_FALSE(second.used_luna);
    CHECK(second.intent.target_agent == "scout");
    CHECK(second.intent.source == "luna");
}

TEST_CASE("context load overlaps the luna call") {
    IntentRouteCache cache;
    std::atomic<int> inflight{0};
    std::atomic<bool> overlapped{false};
    std::string speculative;
    auto cfg = route_cfg();
    cfg.timeout_ms = 400;
    auto out = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            inflight.fetch_add(1, std::memory_order_acq_rel);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            inflight.fetch_sub(1, std::memory_order_acq_rel);
            return joint_result(0.05, 0.90, 0.03, 0.02);
        },
        [&](const std::string& agent) {
            speculative = agent;
            for (int i = 0; i < 40 && inflight.load(std::memory_order_acquire) == 0; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (inflight.load(std::memory_order_acquire) > 0) overlapped.store(true);
        });
    CHECK(overlapped.load());
    CHECK(speculative == "index");
    CHECK(out.timings.tier == "luna");
    CHECK(out.intent.target_agent == "scout");
}

TEST_CASE("two threads can share one cache") {
    IntentRouteCache cache;
    std::atomic<int> calls{0};
    auto cfg = route_cfg();
    cfg.timeout_ms = 400;
    auto luna = [&](const DecisionRequest&, const std::atomic<bool>&) {
        calls.fetch_add(1, std::memory_order_acq_rel);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return joint_result(0.05, 0.90, 0.03, 0.02);
    };
    IntentRouteOutcome a;
    IntentRouteOutcome b;
    std::thread t1([&] {
        a = route_intent_live(input("hello there"), hybrid(), cfg, cache, luna);
    });
    std::thread t2([&] {
        b = route_intent_live(input("hello there"), hybrid(), cfg, cache, luna);
    });
    t1.join();
    t2.join();
    CHECK(a.intent.target_agent == "scout");
    CHECK(b.intent.target_agent == "scout");
    CHECK(calls.load() >= 1);
    auto third = route_intent_live(input("hello there"), hybrid(), cfg, cache, luna);
    CHECK(third.timings.cache_hit);
    CHECK(calls.load() >= 1);
}

TEST_CASE("explicit specialist does not call luna") {
    IntentRouteCache cache;
    int calls = 0;
    auto in = input("hello there");
    in.requested_agent = "vera";
    auto out = route_intent_live(
        in, hybrid(), route_cfg(), cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            ++calls;
            return joint_result(0.05, 0.90, 0.03, 0.02);
        });
    CHECK(calls == 0);
    CHECK(out.timings.tier == "explicit");
    CHECK(out.intent.source == "explicit");
    CHECK(out.intent.target_agent == "vera");
    CHECK_FALSE(intent_route_should_apply(hybrid(), out.intent, "vera"));
}

TEST_CASE("select_executor_model uses difficulty and leaves the agent model alone") {
    auto cfg = intent_route_defaults();
    cfg.strong_model = "strong-id";
    cfg.cheap_model = "cheap-id";
    CHECK(select_executor_model("base", "advisor", 0.9, true, cfg) == "strong-id");
    CHECK(select_executor_model("base", "advisor", 0.1, true, cfg) == "cheap-id");
    cfg.strong_model.clear();
    cfg.cheap_model.clear();
    CHECK(select_executor_model("base", "advisor", 0.9, true, cfg) == "advisor");
    CHECK(select_executor_model("base", "advisor", 0.1, true, cfg).empty());
    CHECK(select_executor_model("base", "", 0.9, true, cfg).empty());
    cfg.default_difficulty = 0.1;
    CHECK(select_executor_model("base", "advisor", 0, false, cfg).empty());
}

TEST_CASE("env parser keeps documented defaults and rejects junk") {
    unsetenv("ARBITER_INTENT_LIVE");
    unsetenv("ARBITER_INTENT_LUNA_MIN_CONFIDENCE");
    unsetenv("ARBITER_INTENT_LUNA_TIMEOUT_MS");
    CHECK_FALSE(intent_live_luna_enabled());
    auto base = intent_route_config_from_env();
    CHECK_FALSE(base.live);
    CHECK(base.luna_min_confidence == doctest::Approx(0.55));
    CHECK(base.timeout_ms == 400);
    CHECK(base.cache_ttl_ms == 60000);
    CHECK(base.cache_entries == 64);
    CHECK(base.default_agent == "index");
    {
        EnvGuard live("ARBITER_INTENT_LIVE", "luna");
        EnvGuard junk("ARBITER_INTENT_LUNA_MIN_CONFIDENCE", "nope");
        EnvGuard timeout("ARBITER_INTENT_LUNA_TIMEOUT_MS", "250");
        CHECK(intent_live_luna_enabled());
        auto cfg = intent_route_config_from_env();
        CHECK(cfg.live);
        CHECK(cfg.luna_min_confidence == doctest::Approx(0.55));
        CHECK(cfg.timeout_ms == 250);
    }
    CHECK_FALSE(intent_live_luna_enabled());
}
