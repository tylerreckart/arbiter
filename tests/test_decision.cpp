// tests/test_decision.cpp — Decision interface, logprob normalization, and
// the Luna request shape. No network. Live classification is
// arbiter-decision-smoke, and only when OPENROUTER_API_KEY is set.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "decision.h"

#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

using namespace arbiter;

static DecisionOption opt(const char* id, const char* label, const char* desc = "") {
    DecisionOption o;
    o.id = id;
    o.label = label;
    o.description = desc;
    return o;
}

static std::vector<DecisionOption> three() {
    return {opt("scout", "A", "research"),
            opt("vera", "B", "review"),
            opt("forge", "C", "ops")};
}

TEST_CASE("confidence is (p_max - 1/n) / (1 - 1/n)") {
    CHECK(decision_confidence(1.0, 4) == doctest::Approx(1.0));
    CHECK(decision_confidence(0.25, 4) == doctest::Approx(0.0));
    CHECK(decision_confidence(0.625, 4) == doctest::Approx(0.5));
    CHECK(decision_confidence(0.0, 4) == doctest::Approx(0.0));
    CHECK(decision_confidence(1.0, 1) == doctest::Approx(1.0));
    CHECK(decision_confidence(0.5, 2) == doctest::Approx(0.0));
}

TEST_CASE("single-token labels are the first 20 letters") {
    CHECK(single_token_label(0) == "A");
    CHECK(single_token_label(19) == "T");
    CHECK(single_token_label(20).empty());
    CHECK(is_single_token_label("A"));
    CHECK_FALSE(is_single_token_label("scout"));
    CHECK_FALSE(is_single_token_label(""));
}

TEST_CASE("softmax normalizes over observed option labels only") {
    TokenLogprobs tok;
    tok.token = "A";
    tok.logprob = std::log(0.6);
    tok.has_logprob = true;
    tok.top_logprobs = {
        {"A", std::log(0.6)},
        {" B", std::log(0.3)},
        {"c", std::log(0.1)},
        {"Hello", -0.2},
    };
    auto result = classify_logprobs(three(), {tok});
    REQUIRE(result.ok);
    CHECK(result.choice == "scout");
    CHECK(result.choice_label == "A");
    CHECK(result.probability == doctest::Approx(0.6));
    CHECK(result.distribution[0].second == doctest::Approx(0.6));
    CHECK(result.distribution[1].second == doctest::Approx(0.3));
    CHECK(result.distribution[2].second == doctest::Approx(0.1));
    CHECK(result.labels_observed == 3);
    CHECK(result.sampled_token == "A");
    CHECK(result.confidence == doctest::Approx(decision_confidence(0.6, 3)));
}

TEST_CASE("a missing label is zero and the others are renormalized") {
    TokenLogprobs tok;
    tok.token = "A";
    tok.has_logprob = true;
    tok.logprob = std::log(0.6);
    tok.top_logprobs = {{"A", std::log(0.6)}, {"B", std::log(0.3)}};
    auto result = classify_logprobs(three(), {tok});
    REQUIRE(result.ok);
    CHECK(result.labels_observed == 2);
    CHECK(result.distribution[0].first == "scout");
    CHECK(result.distribution[0].second == doctest::Approx(0.6 / 0.9));
    CHECK(result.distribution[1].second == doctest::Approx(0.3 / 0.9));
    CHECK(result.distribution[2].second == doctest::Approx(0.0));
    double sum = 0;
    for (const auto& item : result.distribution) sum += item.second;
    CHECK(sum == doctest::Approx(1.0));
}

TEST_CASE("duplicate tokens keep the higher logprob") {
    TokenLogprobs tok;
    tok.token = "A";
    tok.has_logprob = true;
    tok.logprob = -1.0;
    tok.top_logprobs = {{"A", -1.0}, {" A", -0.2}, {"B", -0.2}};
    auto options = std::vector<DecisionOption>{opt("scout", "A"), opt("vera", "B")};
    auto result = classify_logprobs(options, {tok});
    REQUIRE(result.ok);
    CHECK(result.probability == doctest::Approx(0.5));
    CHECK(result.choice == "scout");
}

TEST_CASE("no matching label is an error with an empty distribution") {
    TokenLogprobs tok;
    tok.token = "Hello";
    tok.has_logprob = true;
    tok.logprob = -0.1;
    tok.top_logprobs = {{"Hello", -0.1}, {"World", -2.0}};
    auto result = classify_logprobs(three(), {tok});
    CHECK_FALSE(result.ok);
    CHECK(result.distribution.empty());
    CHECK(result.error.find("no option label") != std::string::npos);
    CHECK(result.sampled_token == "Hello");
}

TEST_CASE("empty logprobs are not treated as a confident choice") {
    auto result = classify_logprobs(three(), {});
    CHECK_FALSE(result.ok);
    CHECK(result.choice.empty());
    CHECK(result.distribution.empty());
}

TEST_CASE("prompt presents single-token labels, not agent ids, as the reply") {
    DecisionRequest req;
    req.state = "Look up the RISC-V memory model";
    req.question = "Which specialist should handle this request?";
    req.options = three();
    std::string prompt = luna_decision_prompt(req);
    CHECK(prompt.find("A. scout") != std::string::npos);
    CHECK(prompt.find("B. vera") != std::string::npos);
    CHECK(prompt.find("C. forge") != std::string::npos);
    CHECK(prompt.find("Reply with exactly one option label.") != std::string::npos);
    CHECK(prompt.find(req.state) != std::string::npos);
    CHECK(prompt.find(req.question) != std::string::npos);
    const auto options_at = prompt.find("Options:");
    const auto state_at = prompt.find("State:");
    CHECK(options_at != std::string::npos);
    CHECK(state_at != std::string::npos);
    CHECK(options_at < state_at);
}

TEST_CASE("default openai body does not grow logprobs or reasoning") {
    ApiRequest req;
    req.model = "openai/gpt-6-luna";
    req.max_tokens = 128;
    Message m;
    m.role = "user";
    m.content = "hello";
    req.messages.push_back(m);

    Provider prov;
    prov.name = "openrouter";
    prov.format = Provider::FORMAT_OPENAI_CHAT;
    auto body = ApiClient::build_body_openai(prov, req, false);
    CHECK(body.find("\"logprobs\"") == std::string::npos);
    CHECK(body.find("top_logprobs") == std::string::npos);
    CHECK(body.find("reasoning") == std::string::npos);
    CHECK(body.find("\"max_tokens\":128") != std::string::npos);
}

TEST_CASE("luna request asks for one token, logprobs, and reasoning off") {
    ApiRequest req;
    req.model = "openai/gpt-6-luna";
    req.max_tokens = 1;
    req.include_temperature = false;
    req.logprobs = true;
    req.top_logprobs = 50;
    req.reasoning_effort = "none";
    req.reasoning_exclude = true;
    Message m;
    m.role = "user";
    m.content = "A or B";
    req.messages.push_back(m);

    Provider prov;
    prov.name = "openrouter";
    auto body = ApiClient::build_body_openai(prov, req, false);
    CHECK(body.find("\"model\":\"openai/gpt-6-luna\"") != std::string::npos);
    CHECK(body.find("\"max_tokens\":1") != std::string::npos);
    CHECK(body.find("\"max_completion_tokens\":1") != std::string::npos);
    CHECK(body.find("\"logprobs\":true") != std::string::npos);
    CHECK(body.find("\"top_logprobs\":20") != std::string::npos);
    CHECK(body.find("\"top_logprobs\":50") == std::string::npos);
    CHECK(body.find("\"reasoning_effort\":\"none\"") != std::string::npos);
    CHECK(body.find("\"effort\":\"none\"") != std::string::npos);
    CHECK(body.find("\"exclude\":true") != std::string::npos);
    CHECK(body.find("temperature") == std::string::npos);
}

TEST_CASE("reasoning settings stay off the ollama body") {
    ApiRequest req;
    req.model = "ollama/llama3";
    req.logprobs = true;
    req.top_logprobs = 5;
    req.reasoning_effort = "none";
    Message m;
    m.role = "user";
    m.content = "A";
    req.messages.push_back(m);

    Provider prov;
    prov.name = "ollama";
    auto body = ApiClient::build_body_openai(prov, req, false);
    CHECK(body.find("\"logprobs\":true") != std::string::npos);
    CHECK(body.find("\"top_logprobs\":5") != std::string::npos);
    CHECK(body.find("reasoning") == std::string::npos);
    CHECK(body.find("max_completion_tokens") == std::string::npos);
}

TEST_CASE("openai parser reads logprobs, served model, and cost") {
    const char* body = R"({
        "model": "openai/gpt-6-luna-20260922",
        "choices": [{
            "finish_reason": "length",
            "message": {"role": "assistant", "content": "A"},
            "logprobs": {
                "content": [{
                    "token": "A",
                    "logprob": -0.1053605156578263,
                    "top_logprobs": [
                        {"token": "A", "logprob": -0.1053605156578263},
                        {"token": "B", "logprob": -2.302585092994046}
                    ]
                }]
            }
        }],
        "usage": {
            "prompt_tokens": 40,
            "completion_tokens": 1,
            "cost": "0.000021"
        }
    })";
    auto resp = ApiClient::parse_body_openai(body);
    CHECK(resp.ok);
    CHECK(resp.content == "A");
    CHECK(resp.served_model == "openai/gpt-6-luna-20260922");
    CHECK(resp.has_cost);
    CHECK(resp.cost_usd == doctest::Approx(0.000021));
    CHECK(resp.input_tokens == 40);
    CHECK(resp.output_tokens == 1);
    REQUIRE(resp.logprobs.size() == 1);
    CHECK(resp.logprobs[0].token == "A");
    CHECK(resp.logprobs[0].has_logprob);
    CHECK(resp.logprobs[0].logprob == doctest::Approx(-0.1053605156578263));
    REQUIRE(resp.logprobs[0].top_logprobs.size() == 2);
    CHECK(resp.logprobs[0].top_logprobs[1].token == "B");
}

TEST_CASE("error bodies still keep model and cost, and null logprobs parse") {
    const char* body = R"({
        "model": "openai/gpt-6-luna",
        "error": {
            "message": "logprobs are not supported with reasoning models.",
            "type": "invalid_request_error"
        },
        "usage": {"total_cost": 0.00001}
    })";
    auto resp = ApiClient::parse_body_openai(body);
    CHECK_FALSE(resp.ok);
    CHECK(resp.error.find("not supported") != std::string::npos);
    CHECK(resp.served_model == "openai/gpt-6-luna");
    CHECK(resp.has_cost);
    CHECK(resp.cost_usd == doctest::Approx(0.00001));
    CHECK(resp.logprobs.empty());

    auto plain = ApiClient::parse_body_openai(
        R"({"choices":[{"message":{"content":"hi"},"logprobs":null}],"usage":{"prompt_tokens":3,"completion_tokens":1}})");
    CHECK(plain.ok);
    CHECK(plain.content == "hi");
    CHECK_FALSE(plain.has_cost);
    CHECK(plain.logprobs.empty());
    CHECK(plain.served_model.empty());
}

TEST_CASE("luna backend normalizes a mocked logprob response") {
    int calls = 0;
    LunaLogprobBackend backend([&](const ApiRequest& req) {
        ++calls;
        CHECK(req.model == kLunaModelId);
        CHECK(req.max_tokens == 1);
        CHECK(req.logprobs);
        CHECK(req.top_logprobs == 20);
        CHECK(req.reasoning_effort == "none");
        CHECK_FALSE(req.include_temperature);
        CHECK(req.messages.size() == 1);
        CHECK(req.messages[0].content.find("A. scout") != std::string::npos);
        ApiResponse resp;
        resp.ok = true;
        resp.served_model = "openai/gpt-6-luna";
        resp.has_cost = true;
        resp.cost_usd = 0.000012;
        resp.content = "A";
        TokenLogprobs tok;
        tok.token = "A";
        tok.has_logprob = true;
        tok.logprob = std::log(0.7);
        tok.top_logprobs = {
            {"A", std::log(0.7)},
            {"B", std::log(0.2)},
            {"C", std::log(0.1)},
        };
        resp.logprobs.push_back(tok);
        return resp;
    });

    auto result = decide(backend, "cite the RISC-V memory model",
                         "Which specialist should handle this request?",
                         three());
    CHECK(calls == 1);
    REQUIRE(result.ok);
    CHECK(result.backend == "luna-logprob");
    CHECK(result.choice == "scout");
    CHECK(result.probability == doctest::Approx(0.7));
    CHECK(result.confidence == doctest::Approx(decision_confidence(0.7, 3)));
    CHECK(result.cost_known);
    CHECK(result.cost_usd == doctest::Approx(0.000012));
    CHECK(result.latency_ms >= 0);
    CHECK(result.model == "openai/gpt-6-luna");
    CHECK(result.attempts == 1);
    CHECK(result.labels_observed == 3);
}

TEST_CASE("a logprobs rejection is returned, not replaced with a distribution") {
    LunaLogprobBackend backend([](const ApiRequest&) {
        ApiResponse resp;
        resp.ok = false;
        resp.error = "logprobs are not supported with reasoning models.";
        resp.error_type = "invalid_request_error";
        resp.served_model = "openai/gpt-6-luna";
        resp.has_cost = true;
        resp.cost_usd = 0.000004;
        return resp;
    });
    auto result = backend.decide([&] {
        DecisionRequest req;
        req.state = "review this diff";
        req.question = "Which specialist?";
        req.options = three();
        return req;
    }());
    CHECK_FALSE(result.ok);
    CHECK(result.distribution.empty());
    CHECK(result.choice.empty());
    CHECK(result.error.find("not supported") != std::string::npos);
    CHECK(result.cost_known);
    CHECK(result.cost_usd == doctest::Approx(0.000004));
    CHECK(result.model == "openai/gpt-6-luna");
    CHECK(result.attempts == 1);
}

TEST_CASE("top_logprobs rejection retries once at 1") {
    int calls = 0;
    LunaLogprobBackend backend([&](const ApiRequest& req) {
        ++calls;
        ApiResponse resp;
        if (calls == 1) {
            CHECK(req.top_logprobs == 20);
            resp.ok = false;
            resp.error = "Invalid top_logprobs: only 0 or 1 is supported";
            resp.has_cost = true;
            resp.cost_usd = 0.000001;
            return resp;
        }
        CHECK(req.top_logprobs == 1);
        resp.ok = true;
        resp.served_model = "openai/gpt-6-luna";
        resp.has_cost = true;
        resp.cost_usd = 0.000002;
        TokenLogprobs tok;
        tok.token = "B";
        tok.has_logprob = true;
        tok.logprob = -0.01;
        tok.top_logprobs = {{"B", -0.01}};
        resp.logprobs.push_back(tok);
        return resp;
    });
    DecisionRequest req;
    req.state = "review the patch";
    req.question = "Which specialist?";
    req.options = three();
    auto result = backend.decide(req);
    CHECK(calls == 2);
    REQUIRE(result.ok);
    CHECK(result.choice == "vera");
    CHECK(result.top_logprobs == 1);
    CHECK(result.attempts == 2);
    CHECK(result.labels_observed == 1);
    CHECK(result.distribution[0].second == doctest::Approx(0.0));
    CHECK(result.distribution[1].second == doctest::Approx(1.0));
    CHECK(result.cost_known);
    CHECK(result.cost_usd == doctest::Approx(0.000003));
    CHECK(result.note.find("top_logprobs=1") != std::string::npos);
}

TEST_CASE("roster options are single-token labels in id order") {
    std::vector<IntentRosterEntry> roster = {
        {"vera", "code-reviewer", "Inspect real code", {}},
        {"index", "master", "Route work", {}},
        {"scout", "research-analyst", "Research with depth", {}},
        {"", "ignored", "no id", {}},
    };
    auto options = options_from_roster(roster);
    REQUIRE(options.size() == 2);
    CHECK(options[0].id == "scout");
    CHECK(options[0].label == "A");
    CHECK(options[1].id == "vera");
    CHECK(options[1].label == "B");
    CHECK(options[0].description.find("research-analyst") != std::string::npos);
}

TEST_CASE("shadow flag defaults off and does not touch the heuristic") {
    const char* prev = std::getenv("ARBITER_INTENT_SHADOW");
    ::unsetenv("ARBITER_INTENT_SHADOW");
    CHECK_FALSE(intent_shadow_luna_enabled());

    ::setenv("ARBITER_INTENT_SHADOW", "off", 1);
    CHECK_FALSE(intent_shadow_luna_enabled());
    ::setenv("ARBITER_INTENT_SHADOW", "Luna", 1);
    CHECK(intent_shadow_luna_enabled());

    // Named map, then move. `ApiClient({{...}})` is ambiguous on GCC 11
    // (ubuntu-22.04) between the map constructor and the deleted copy.
    std::map<std::string, std::string> keys{{"openrouter", "dummy-key-no-network"}};
    ApiClient client(std::move(keys));
    Intent hint;
    hint.kind = "research";
    hint.source = "heuristic";
    hint.target_agent = "scout";
    hint.confidence = 0.91;
    std::vector<IntentRosterEntry> roster = {
        {"scout", "research-analyst", "Research", {}},
        {"vera", "code-reviewer", "Review", {}},
    };
    shadow_intent_decision(client, "cite the paper", roster, hint);
    CHECK(hint.kind == "research");
    CHECK(hint.source == "heuristic");
    CHECK(hint.target_agent == "scout");
    CHECK(hint.confidence == doctest::Approx(0.91));

    ::unsetenv("ARBITER_INTENT_SHADOW");
    shadow_intent_decision(client, "cite the paper", roster, hint);
    CHECK(hint.target_agent == "scout");

    if (prev) ::setenv("ARBITER_INTENT_SHADOW", prev, 1);
    else ::unsetenv("ARBITER_INTENT_SHADOW");
}
