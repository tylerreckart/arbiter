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
// Confidence is (p_max - 1/n) / (1 - 1/n), with n = options.size().
// Probabilities are a softmax over the option labels that appeared in
// logprobs.content[0].top_logprobs (plus the sampled token). Labels that
// did not appear are reported as 0 and are not given invented mass.
//
// OpenRouter's catalogue entry for openai/gpt-6-luna (checked 2026-10-13)
// does not list logprobs or top_logprobs. OpenAI's own API returns them
// for Luna only when reasoning is off, and rejects them while reasoning
// is on. The backend sends reasoning.effort=none plus logprobs anyway.
// A rejection is an error result — never a fabricated distribution.

#include "api_client.h"
#include "intent.h"

#include <functional>
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
    double probability = 0;    // P(choice) after normalizing over options
    // id → probability, in option order. Sums to 1 when ok. Unobserved
    // labels contribute 0.
    std::vector<std::pair<std::string, double>> distribution;
    double confidence = 0;     // (p_max - 1/n) / (1 - 1/n), clamped to [0, 1]
    double cost_usd = 0;
    bool cost_known = false;
    double latency_ms = 0;
    std::string model;            // served model when the provider named one
    std::string requested_model;
    std::string sampled_token;    // the one token the model actually wrote
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
double decision_confidence(double p_max, std::size_t n);

// Softmax over option labels found in the first generated token's
// logprobs. ok is false when no option label was observed.
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
