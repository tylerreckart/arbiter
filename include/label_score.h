#pragma once
// arbiter/include/label_score.h — Closed-set next-token label distribution.
//
// A local model scores a short state against single-letter labels. Callers
// act only when the renormalized distribution is peaked; otherwise they
// keep the generative path they already had. The scorer never writes prose.
//
// Hosted providers are not consulted. An empty model, a non-ollama id, a
// missing label in top_logprobs, or a transport error yields ok=false, and
// callers fall through.

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace arbiter {

class ApiClient;

// Peak must clear this floor before a margin check can skip a generative
// call. The margin itself comes from the environment and defaults to off.
inline constexpr double kDecisionPeakFloor = 0.9;

// Intent utterances passed to the scorer are clipped here. The heuristic
// and the advisor completion still see the full text.
inline constexpr std::size_t kIntentDecisionStateMaxBytes = 2048;

struct LabelSpec {
    char letter = 0;          // one ASCII letter
    std::string name;         // stable id callers switch on ("silent", "research")
    std::string description;  // the only instruction the model is given
};

struct TokenLogprob {
    std::string token;
    double logprob = 0;
};

struct LabelDistribution {
    bool ok = false;
    std::string argmax;   // LabelSpec::name; empty when !ok
    double peak = 0;      // probability of argmax after renormalizing
    double margin = 0;    // peak − second; 0 when the top two tie
    // name → probability, in spec order. Empty when !ok.
    std::vector<std::pair<std::string, double>> probs;
};

// Softmax over the supplied labels only. ok is false when any letter is
// absent, a letter is duplicated, or a spec is not a single ASCII letter.
// Tokens match a letter only when, after trimming whitespace, they are
// exactly that letter. "Silent" does not count as S.
LabelDistribution distribution_from_logprobs(
    const std::vector<LabelSpec>& specs,
    const std::vector<TokenLogprob>& tokens);

// Parse choices[0].logprobs.content[0].top_logprobs from an OpenAI-compatible
// chat completion body. Any other shape returns ok=false.
LabelDistribution distribution_from_openai_body(
    const std::vector<LabelSpec>& specs,
    const std::string& body);

// True when the distribution is peaked enough to skip a generative call.
// margin_floor <= 0 never passes: the filter ships dark.
bool label_distribution_is_extreme(const LabelDistribution& d,
                                   double margin_floor);

const char* label_score_system_prompt();

std::string label_score_user_prompt(const std::string& state,
                                    const std::vector<LabelSpec>& specs);

// Clip to max_bytes without splitting a trailing UTF-8 sequence.
std::string clip_utf8(std::string text, std::size_t max_bytes);

struct DecisionFilterConfig {
    std::string model;  // empty, or an ollama/… id
    double presence_silence_margin = 0;
    double intent_route_margin = 0;

    bool enabled_for_presence() const;
    bool enabled_for_intent() const;
};

// Reads ARBITER_DECISION_MODEL, ARBITER_PRESENCE_SILENCE_MARGIN, and
// ARBITER_INTENT_ROUTE_MARGIN. Unset or unparsable margins are 0 (off).
// Values are clamped to [0, 1].
DecisionFilterConfig decision_filter_from_env();

// Same parse, for tests that must not touch the process environment.
DecisionFilterConfig decision_filter_from_values(const char* model,
                                                 const char* presence_margin,
                                                 const char* intent_margin);

// Closed kind set for the intent filter, including unknown. multi is
// omitted: a multi-cue heuristic result never reaches the scorer.
std::vector<LabelSpec> intent_decision_labels();

using LabelScorer = std::function<LabelDistribution(
    const std::string& state, const std::vector<LabelSpec>& specs)>;

// Network call. Defined in api_client.cpp. Non-ollama models and failed
// completions return a distribution with ok=false and do not throw.
struct LabelScoreCall {
    LabelDistribution distribution;
    // called is true only when complete() ran, so callers can attribute
    // cost. A refused non-ollama id leaves called false.
    bool called = false;
    bool response_ok = false;
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;
    int cache_creation_tokens = 0;
    std::string model;
};

LabelScoreCall score_labels(ApiClient& client,
                            const std::string& model,
                            const std::string& state,
                            const std::vector<LabelSpec>& specs);

}  // namespace arbiter
