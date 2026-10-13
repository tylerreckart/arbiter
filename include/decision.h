#pragma once
// arbiter/include/decision.h — Closed-set decision interface.
//
// decide(state, question, options) → choice, probability, distribution,
// confidence, cost, latency. Backends are swappable. This PR implements
// LunaLogprobBackend (OpenRouter openai/gpt-6-luna, reasoning off, one
// output token, logprobs). Later backends can implement DecisionBackend
// without changing callers:
//   • Jev (TypeSafe AI), which returns a probability per option directly
//   • a structured-output classifier
//   • the heuristic in src/intent.cpp
//
// Confidence is (p - 1/n) / (1 - 1/n). p is exp(logprob) of the winning
// observed label — the token's own probability, not a softmax over the
// labels the provider happened to return. n is options.size(), not the
// number of labels observed. Unseen labels are unknown (nullopt), not 0.
// A response that contains only the sampled token is therefore not
// probability 1 or confidence 1 unless that token's logprob is 0.
//
// OpenRouter's catalogue entry for openai/gpt-6-luna (checked 2026-10-13)
// does not list logprobs or top_logprobs. A live call with reasoning off
// does return logprobs, but only one top_logprobs entry no matter what
// top_logprobs was requested. Sending provider.require_parameters=true
// 404s, so the parameter is unofficial. OpenAI's own API returns logprobs
// for Luna only when reasoning is off. The backend sends
// reasoning.effort=none plus logprobs anyway. A rejection is an error
// result — never a fabricated distribution.

#include "api_client.h"
#include "intent.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace arbiter {

// OpenAI's chat API caps top_logprobs at 20, so a single-token label set
// cannot honestly cover more options than that.
inline constexpr std::size_t kMaxLunaOptions = 20;

// Verified against GET https://openrouter.ai/api/v1/models on 2026-10-13.
inline constexpr const char* kLunaModelId = "openai/gpt-6-luna";

struct DecisionOption {
    std::string id;           // stable id callers switch on ("scout")
    std::string label;        // single ASCII alphanumeric token ("A")
    std::string description;  // shown to the model; not used as the label
};

struct DecisionRequest {
    std::string state;
    std::string question;
    std::vector<DecisionOption> options;
};

struct DecisionResult {
    bool ok = false;
    std::string backend;       // "luna-logprob"
    std::string choice;        // option id
    std::string choice_label;  // the label that won
    // exp(logprob) of the chosen label. Not renormalized over the labels
    // that appeared in this response.
    double probability = 0;
    // id → probability, in option order. nullopt = that label was not in
    // the returned logprobs (unknown, not a measured zero). Known values
    // are exp(logprob) and are not rescaled to sum to 1.
    std::vector<std::pair<std::string, std::optional<double>>> distribution;
    // decision_confidence(probability, options.size()): how far the raw
    // token probability sits above a uniform draw over the full option
    // set. A single observed label does not force this to 1.
    double confidence = 0;
    double cost_usd = 0;
    bool cost_known = false;
    double latency_ms = 0;
    std::string model;            // served model when the provider named one
    std::string requested_model;
    std::string sampled_token;    // the one token the model actually wrote
    // How many option labels appeared in the returned logprobs. This is
    // not the top_logprobs value that was requested. OpenRouter's Luna
    // route has been observed to return 1 here even when 20 were asked for.
    int labels_observed = 0;
    int top_logprobs = 0;         // value used on the call that produced this
    int attempts = 0;
    std::string note;             // e.g. retried because top_logprobs > 1 failed
    std::string error;
};

class DecisionBackend {
public:
    virtual ~DecisionBackend() = default;
    virtual DecisionResult decide(const DecisionRequest& request) = 0;
    virtual const char* name() const = 0;
};

DecisionResult decide(DecisionBackend& backend,
                      const std::string& state,
                      const std::string& question,
                      const std::vector<DecisionOption>& options);

// 'A'..'T' for indexes 0..19. Empty past kMaxLunaOptions.
std::string single_token_label(std::size_t index);
bool is_single_token_label(const std::string& label);

// n == 1 → 1. p_max below the uniform baseline → 0.
// p_max is a raw probability (exp of a logprob, or a caller's direct
// probability). It is not assumed to be a softmax over a subset.
double decision_confidence(double p_max, std::size_t n);

// Map the first generated token's logprobs onto options.
// Each observed label's probability is exp(logprob). Unseen labels are
// nullopt. ok is false when no option label was observed. Confidence is
// decision_confidence(probability, options.size()).
DecisionResult classify_logprobs(const std::vector<DecisionOption>& options,
                                 const std::vector<TokenLogprobs>& content);

std::string luna_decision_system_prompt();
std::string luna_decision_prompt(const DecisionRequest& request);

class LunaLogprobBackend : public DecisionBackend {
public:
    using CompleteFn = std::function<ApiResponse(const ApiRequest&)>;

    explicit LunaLogprobBackend(ApiClient& client,
                                std::string model = kLunaModelId);
    explicit LunaLogprobBackend(CompleteFn complete,
                                std::string model = kLunaModelId);

    DecisionResult decide(const DecisionRequest& request) override;
    const char* name() const override { return "luna-logprob"; }

    // 0 (the default) requests top_logprobs=20. Clamped to 1..20.
    void set_top_logprobs(int n) { top_logprobs_ = n; }

private:
    ApiClient* client_ = nullptr;
    CompleteFn complete_;
    std::string model_;
    int top_logprobs_ = 0;

    ApiResponse complete(const ApiRequest& req);
};

// Roster entries become options with single-token labels, sorted by id.
// "index" and empty ids are skipped. More than kMaxLunaOptions entries
// are kept; decide() then refuses them rather than dropping specialists.
std::vector<DecisionOption> options_from_roster(
    const std::vector<IntentRosterEntry>& roster);

// True when ARBITER_INTENT_SHADOW is luna / on / true / 1.
bool intent_shadow_luna_enabled();

// Logs a Luna decision next to an already-computed intent. Does not modify
// `heuristic` and does not throw. No-op unless the shadow flag is set.
// ARBITER_INTENT_SHADOW_MODEL overrides the model id.
void shadow_intent_decision(ApiClient& client,
                            const std::string& text,
                            const std::vector<IntentRosterEntry>& roster,
                            const Intent& heuristic);

} // namespace arbiter
