// tests/test_intent_route.cpp — Tiered live router. No network.
// Luna is a mock. The generative <intent> classifier is not a parameter
// of route_intent_live, so these cases cannot call it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "intent_route.h"
#include "embed_route.h"

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

void wait_luna(const IntentRouteOutcome& out) {
    if (!out.luna_finished) return;
    for (int i = 0; i < 400 &&
         !out.luna_finished->load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

DecisionResult embed_of(const std::string& choice, double confidence, bool ok = true) {
    DecisionResult r;
    r.ok = ok;
    r.choice = choice;
    r.confidence = confidence;
    r.probability = confidence;
    r.latency_ms = 3;
    r.backend = "embed-centroid";
    return r;
}

DecisionResult vera_win() {
    DecisionResult r;
    r.ok = true;
    r.choice = "vera|strong";
    r.distribution = {
        {"scout|cheap", 0.05},
        {"scout|strong", 0.05},
        {"vera|cheap", 0.10},
        {"vera|strong", 0.80},
    };
    r.latency_ms = 12;
    return r;
}

} // namespace

TEST_CASE("embedding absence returns the default agent without waiting for luna") {
    IntentRouteCache cache;
    int calls = 0;
    auto cfg = route_cfg();
    cfg.timeout_ms = 5000;
    auto out = route_intent_live(
        input("Look up primary sources for the 2024 RISC-V memory model"),
        hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            ++calls;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return joint_result(0.05, 0.90, 0.03, 0.02);
        });
    CHECK(out.timings.tier == "fallback");
    CHECK(out.intent.source == "fallback");
    CHECK(out.intent.target_agent.empty());
    CHECK(out.used_luna);
    CHECK(out.luna_finished);
    CHECK_FALSE(out.luna_finished->load(std::memory_order_acquire));
    CHECK(out.timings.total_ms < 200);
    CHECK_FALSE(intent_route_should_apply(hybrid(), out.intent, "index"));
    wait_luna(out);
    CHECK(out.luna_finished->load(std::memory_order_acquire));
    CHECK(calls == 1);
    auto next = route_intent_live(
        input("Look up primary sources for the 2024 RISC-V memory model"),
        hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            ++calls;
            return joint_result(0.05, 0.90, 0.03, 0.02);
        });
    CHECK(next.timings.tier == "cache");
    CHECK(next.timings.cache_hit);
    CHECK(next.intent.target_agent == "scout");
    CHECK(next.intent.source == "luna");
    CHECK(calls == 1);
}

TEST_CASE("background luna carries agent and difficulty into the next route") {
    IntentRouteCache cache;
    int calls = 0;
    DecisionRequest seen;
    auto cfg = route_cfg();
    auto first = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest& req, const std::atomic<bool>&) {
            ++calls;
            seen = req;
            return joint_result(0.05, 0.90, 0.03, 0.02);
        });
    CHECK(first.timings.tier == "fallback");
    CHECK(first.intent.target_agent.empty());
    CHECK(first.used_luna);
    CHECK_FALSE(first.timings.choose_model);
    wait_luna(first);
    CHECK(calls == 1);
    CHECK(has_both_bands(seen, "scout"));
    CHECK(has_both_bands(seen, "vera"));
    CHECK(seen.state == "hello there");
    CHECK(seen.question.find("cheap") != std::string::npos);
    auto out = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            ++calls;
            return DecisionResult{};
        });
    CHECK(calls == 1);
    CHECK(out.timings.tier == "cache");
    CHECK(out.intent.source == "luna");
    CHECK(out.intent.target_agent == "scout");
    CHECK(out.intent.confidence == doctest::Approx(0.9));
    CHECK(out.timings.difficulty_known);
    CHECK(out.timings.difficulty == doctest::Approx(0.92));
    CHECK(out.timings.model_tier == "strong");
    CHECK(out.timings.choose_model);
    CHECK(intent_route_should_apply(hybrid(), out.intent, "index"));
}

TEST_CASE("a low-confidence luna result is the next route") {
    IntentRouteCache cache;
    auto cfg = route_cfg();
    auto first = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            return joint_result(0.40, 0.20, 0.30, 0.10);
        });
    CHECK(first.timings.tier == "fallback");
    wait_luna(first);
    auto out = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            return joint_result(0.05, 0.90, 0.03, 0.02);
        });
    CHECK(out.timings.tier == "cache");
    CHECK(out.intent.source == "luna_guess");
    CHECK(out.intent.target_agent == "scout");
    CHECK(out.intent.confidence == doctest::Approx(0.2));
    CHECK(out.intent.confidence < cfg.luna_min_confidence);
    CHECK(intent_route_should_apply(hybrid(), out.intent, "index"));
    CHECK(out.timings.model_tier == "cheap");
}

TEST_CASE("one observed logprob stays below the luna cutoff on the next route") {
    IntentRouteCache cache;
    auto cfg = route_cfg();
    auto first = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
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
    wait_luna(first);
    auto out = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            return joint_result(0.05, 0.90, 0.03, 0.02);
        });
    CHECK(out.timings.cache_hit);
    CHECK(out.intent.source == "luna_guess");
    CHECK(out.intent.target_agent == "scout");
    CHECK(out.intent.confidence == doctest::Approx(decision_confidence(0.42, 2)));
    CHECK(out.intent.confidence < cfg.luna_min_confidence);
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
    CHECK(first.timings.tier == "fallback");
    CHECK(first.intent.source == "fallback");
    CHECK(first.intent.target_agent.empty());
    CHECK(first.timings.error == "embedding unavailable");
    CHECK_FALSE(intent_route_should_apply(hybrid(), first.intent, "index"));
    wait_luna(first);
    auto logged = cache.find_decision(first.request_hash);
    REQUIRE(logged.has_value());
    CHECK(logged->luna_choice.empty());
    CHECK_FALSE(logged->luna_prob_known);
    auto second = route_intent_live(input("hello there"), hybrid(), cfg, cache, fail);
    wait_luna(second);
    CHECK(calls == 2);
    CHECK(second.timings.tier == "fallback");
    CHECK_FALSE(second.timings.cache_hit);
}

TEST_CASE("timeout cancels luna and the caller has already returned") {
    IntentRouteCache cache;
    std::atomic<bool> saw_cancel{false};
    auto cfg = route_cfg();
    cfg.timeout_ms = 1000;
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
    CHECK(out.luna_finished);
    CHECK_FALSE(out.luna_finished->load(std::memory_order_acquire));
    CHECK(out.timings.total_ms < 200);
    wait_luna(out);
    CHECK(saw_cancel.load());
    CHECK(out.luna_finished->load(std::memory_order_acquire));
    auto again = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            return joint_result(0.05, 0.90, 0.03, 0.02);
        });
    CHECK_FALSE(again.timings.cache_hit);
    wait_luna(again);
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
    wait_luna(first);
    auto second = route_intent_live(input("Hello   There"), hybrid(), cfg, cache, luna);
    CHECK(calls == 1);
    CHECK(first.timings.tier == "fallback");
    CHECK_FALSE(first.timings.cache_hit);
    CHECK(second.timings.tier == "cache");
    CHECK(second.timings.cache_hit);
    CHECK(second.timings.luna_ms == 0);
    CHECK_FALSE(second.used_luna);
    CHECK(second.intent.target_agent == "scout");
    CHECK(second.intent.source == "luna");
    CHECK(first.request_hash == second.request_hash);
    CHECK(first.request_hash.size() == 64);
}

TEST_CASE("context load overlaps the luna call") {
    IntentRouteCache cache;
    std::atomic<int> inflight{0};
    std::atomic<bool> overlapped{false};
    std::string speculative;
    auto cfg = route_cfg();
    cfg.timeout_ms = 5000;
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
    CHECK(out.timings.tier == "fallback");
    CHECK(out.intent.target_agent.empty());
    wait_luna(out);
}

TEST_CASE("two threads can share one cache") {
    IntentRouteCache cache;
    std::atomic<int> calls{0};
    auto cfg = route_cfg();
    cfg.timeout_ms = 5000;
    auto luna = [&](const DecisionRequest&, const std::atomic<bool>&) {
        calls.fetch_add(1, std::memory_order_acq_rel);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
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
    CHECK(a.timings.total_ms < 200);
    CHECK(b.timings.total_ms < 200);
    wait_luna(a);
    wait_luna(b);
    CHECK(calls.load() >= 1);
    auto third = route_intent_live(input("hello there"), hybrid(), cfg, cache, luna);
    CHECK(third.timings.cache_hit);
    CHECK(third.intent.target_agent == "scout");
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
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return joint_result(0.05, 0.90, 0.03, 0.02);
        });
    CHECK(calls == 0);
    CHECK_FALSE(out.used_luna);
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

TEST_CASE("a confident embedding routes and does not call luna") {
    IntentRouteCache cache;
    int luna_calls = 0;
    int embed_calls = 0;
    auto cfg = route_cfg();
    cfg.embed_min_confidence = 0.35;
    auto out = route_intent_live(
        input("Look up primary sources for the 2024 RISC-V memory model"),
        hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            ++luna_calls;
            return joint_result(0.05, 0.90, 0.03, 0.02);
        },
        {},
        [&](const DecisionRequest&) {
            ++embed_calls;
            return embed_of("vera", 0.9);
        });
    CHECK(embed_calls == 1);
    CHECK(luna_calls == 0);
    CHECK_FALSE(out.used_luna);
    CHECK(out.timings.tier == "embed");
    CHECK(out.intent.source == "embed");
    CHECK(out.intent.target_agent == "vera");
    CHECK_FALSE(out.timings.choose_model);
    CHECK(intent_route_should_apply(hybrid(), out.intent, "index"));
    auto rec = cache.find_decision(out.request_hash);
    REQUIRE(rec.has_value());
    CHECK(rec->embed_choice == "vera");
    CHECK(rec->embed_prob == doctest::Approx(0.9));
    CHECK(rec->embed_prob_known);
    CHECK(rec->luna_choice.empty());
    CHECK(rec->final_agent == "vera");
    CHECK(rec->route_ms == doctest::Approx(out.timings.total_ms));
    CHECK(rec->route_ms < 200);
}

TEST_CASE("low embedding confidence keeps the embed choice and updates the cache") {
    IntentRouteCache cache;
    int luna_calls = 0;
    auto cfg = route_cfg();
    cfg.embed_min_confidence = 0.35;
    auto out = route_intent_live(
        input("Look up primary sources for the 2024 RISC-V memory model"),
        hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            ++luna_calls;
            return joint_result(0.05, 0.90, 0.03, 0.02);
        },
        {},
        [&](const DecisionRequest&) { return embed_of("vera", 0.1); });
    CHECK(out.timings.tier == "embed");
    CHECK(out.intent.source == "embed");
    CHECK(out.intent.target_agent == "vera");
    CHECK(out.intent.confidence == doctest::Approx(0.1));
    CHECK(out.timings.embed_ms == doctest::Approx(3));
    CHECK(out.used_luna);
    CHECK(out.timings.total_ms < 200);
    wait_luna(out);
    CHECK(luna_calls == 1);
    auto rec = cache.find_decision(out.request_hash);
    REQUIRE(rec.has_value());
    CHECK(rec->embed_choice == "vera");
    CHECK(rec->embed_prob == doctest::Approx(0.1));
    CHECK(rec->luna_choice == "scout");
    CHECK(rec->luna_prob_known);
    CHECK(rec->luna_prob == doctest::Approx(0.95));
    CHECK(rec->luna_ms == doctest::Approx(12));
    CHECK(rec->final_agent == "vera");
    auto next = route_intent_live(
        input("Look up primary sources for the 2024 RISC-V memory model"),
        hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            ++luna_calls;
            return joint_result(0.05, 0.90, 0.03, 0.02);
        },
        {},
        [&](const DecisionRequest&) { return embed_of("vera", 0.1); });
    CHECK(next.timings.cache_hit);
    CHECK(next.intent.target_agent == "scout");
    CHECK(luna_calls == 1);
}

TEST_CASE("an embedding failure routes to the default agent and still starts luna") {
    IntentRouteCache cache;
    int luna_calls = 0;
    auto cfg = route_cfg();
    auto out = route_intent_live(
        input("Look up primary sources for the 2024 RISC-V memory model"),
        hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            ++luna_calls;
            return joint_result(0.05, 0.90, 0.03, 0.02);
        },
        {},
        [&](const DecisionRequest&) { return embed_of("", 0, false); });
    CHECK(out.timings.tier == "fallback");
    CHECK(out.intent.target_agent.empty());
    CHECK(out.intent.source == "fallback");
    CHECK(out.used_luna);
    wait_luna(out);
    CHECK(luna_calls == 1);
    auto next = route_intent_live(
        input("Look up primary sources for the 2024 RISC-V memory model"),
        hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            ++luna_calls;
            return DecisionResult{};
        });
    CHECK(next.timings.cache_hit);
    CHECK(next.intent.target_agent == "scout");
    CHECK(luna_calls == 1);
}

TEST_CASE("a cached luna decision is the route for the next similar request") {
    IntentRouteCache cache;
    auto cfg = route_cfg();
    auto first = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            return joint_result(0.05, 0.90, 0.03, 0.02);
        });
    wait_luna(first);
    int luna_calls = 0;
    auto out = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            ++luna_calls;
            return joint_result(0.05, 0.02, 0.03, 0.90);
        },
        {},
        [&](const DecisionRequest&) { return embed_of("vera", 0.95); });
    CHECK(luna_calls == 0);
    CHECK(out.timings.tier == "cache");
    CHECK(out.intent.target_agent == "scout");
    CHECK(out.intent.source == "luna");
}

TEST_CASE("the decision log records the advisor verdict later") {
    IntentRouteCache cache;
    auto cfg = route_cfg();
    auto out = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            return joint_result(0.05, 0.90, 0.03, 0.02);
        },
        {},
        [&](const DecisionRequest&) { return embed_of("vera", 0.9); });
    auto rec = cache.find_decision(out.request_hash);
    REQUIRE(rec.has_value());
    CHECK(rec->advisor_verdict.empty());
    note_intent_advisor(cache, out.request_hash, "redirect");
    rec = cache.find_decision(out.request_hash);
    REQUIRE(rec.has_value());
    CHECK(rec->advisor_verdict == "redirect");
    CHECK(rec->final_agent == "vera");
    CHECK(rec->tier == "embed");
}

TEST_CASE("luna reroute stays off unless the flag is set and luna already disagreed") {
    IntentRouteCache cache;
    auto cfg = route_cfg();
    cfg.luna_reroute = false;
    cfg.timeout_ms = 5000;
    auto out = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            return joint_result(0.05, 0.90, 0.03, 0.02);
        },
        {},
        [&](const DecisionRequest&) { return embed_of("vera", 0.1); });
    auto early = cache.poll_reroute(out.request_hash, cfg);
    CHECK_FALSE(early.apply);
    wait_luna(out);
    auto off = cache.poll_reroute(out.request_hash, cfg);
    CHECK_FALSE(off.apply);
    auto rec = cache.find_decision(out.request_hash);
    REQUIRE(rec.has_value());
    CHECK(rec->final_agent == "vera");
    CHECK(rec->luna_choice == "scout");

    cfg.luna_reroute = true;
    auto on = cache.poll_reroute(out.request_hash, cfg);
    CHECK(on.apply);
    CHECK(on.agent == "scout");
    CHECK(on.source == "luna");
    CHECK(on.confidence == doctest::Approx(0.9));
    rec = cache.find_decision(out.request_hash);
    REQUIRE(rec.has_value());
    CHECK(rec->final_agent == "scout");
    auto again = cache.poll_reroute(out.request_hash, cfg);
    CHECK_FALSE(again.apply);
}

TEST_CASE("luna reroute waits until the background call has finished") {
    IntentRouteCache cache;
    auto cfg = route_cfg();
    cfg.luna_reroute = true;
    cfg.timeout_ms = 5000;
    auto out = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            return joint_result(0.05, 0.90, 0.03, 0.02);
        },
        {},
        [&](const DecisionRequest&) { return embed_of("vera", 0.1); });
    auto pending = cache.poll_reroute(out.request_hash, cfg);
    CHECK_FALSE(pending.apply);
    CHECK(out.timings.total_ms < 200);
    wait_luna(out);
    auto ready = cache.poll_reroute(out.request_hash, cfg);
    CHECK(ready.apply);
    CHECK(ready.agent == "scout");
}

TEST_CASE("luna reroute ignores a low-confidence disagreement and an agreement") {
    IntentRouteCache cache;
    auto cfg = route_cfg();
    cfg.luna_reroute = true;
    auto weak = route_intent_live(
        input("hello there"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            return joint_result(0.40, 0.20, 0.30, 0.10);
        },
        {},
        [&](const DecisionRequest&) { return embed_of("vera", 0.1); });
    wait_luna(weak);
    auto weak_rr = cache.poll_reroute(weak.request_hash, cfg);
    CHECK_FALSE(weak_rr.apply);

    auto agree = route_intent_live(
        input("review this diff"), hybrid(), cfg, cache,
        [&](const DecisionRequest&, const std::atomic<bool>&) {
            return vera_win();
        },
        {},
        [&](const DecisionRequest&) { return embed_of("vera", 0.1); });
    wait_luna(agree);
    auto agree_rr = cache.poll_reroute(agree.request_hash, cfg);
    CHECK_FALSE(agree_rr.apply);
    auto rec = cache.find_decision(agree.request_hash);
    REQUIRE(rec.has_value());
    CHECK(rec->luna_choice == "vera");
    CHECK(rec->final_agent == "vera");
}

TEST_CASE("env parser keeps documented defaults and rejects junk") {
    unsetenv("ARBITER_INTENT_LIVE");
    unsetenv("ARBITER_INTENT_LUNA_MIN_CONFIDENCE");
    unsetenv("ARBITER_INTENT_LUNA_TIMEOUT_MS");
    unsetenv("ARBITER_INTENT_LUNA_REROUTE");
    CHECK_FALSE(intent_live_luna_enabled());
    auto base = intent_route_config_from_env();
    CHECK_FALSE(base.live);
    CHECK(base.luna_min_confidence == doctest::Approx(0.55));
    CHECK(base.timeout_ms == 2000);
    CHECK_FALSE(base.luna_reroute);
    CHECK(base.cache_ttl_ms == 60000);
    CHECK(base.cache_entries == 64);
    CHECK(base.default_agent == "index");
    CHECK(base.embed_min_confidence == doctest::Approx(kEmbedDefaultMinConfidence));
    CHECK(base.embed_temperature == doctest::Approx(kEmbedDefaultTemperature));
    {
        EnvGuard live("ARBITER_INTENT_LIVE", "luna");
        EnvGuard junk("ARBITER_INTENT_LUNA_MIN_CONFIDENCE", "nope");
        EnvGuard timeout("ARBITER_INTENT_LUNA_TIMEOUT_MS", "250");
        EnvGuard reroute_junk("ARBITER_INTENT_LUNA_REROUTE", "nope");
        CHECK(intent_live_luna_enabled());
        auto cfg = intent_route_config_from_env();
        CHECK(cfg.live);
        CHECK(cfg.luna_min_confidence == doctest::Approx(0.55));
        CHECK(cfg.timeout_ms == 250);
        CHECK_FALSE(cfg.luna_reroute);
    }
    {
        EnvGuard reroute("ARBITER_INTENT_LUNA_REROUTE", "1");
        auto cfg = intent_route_config_from_env();
        CHECK(cfg.luna_reroute);
    }
    {
        EnvGuard junk("ARBITER_INTENT_EMBED_TEMPERATURE", "nope");
        EnvGuard cutoff("ARBITER_INTENT_EMBED_MIN_CONFIDENCE", "0.4");
        auto cfg = intent_route_config_from_env();
        CHECK(cfg.embed_temperature == doctest::Approx(kEmbedDefaultTemperature));
        CHECK(cfg.embed_min_confidence == doctest::Approx(0.4));
    }
    CHECK_FALSE(intent_live_luna_enabled());
    CHECK_FALSE(intent_route_config_from_env().luna_reroute);
}
