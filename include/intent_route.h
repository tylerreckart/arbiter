#pragma once
// arbiter/include/intent_route.h — Latency-first ingress routing.
//
// Live mode (ARBITER_INTENT_LIVE=luna, default off) replaces the full-LLM
// intent fallback. The first pass is the local embedding router when its
// model is loaded. Below that cutoff, one Luna decide() runs. The heuristic
// decides only when the embedding model is absent.
//
//   1. Embedding centroids. A match at embed_min_confidence returns
//      immediately and does not call Luna or the heuristic.
//   2. One Luna decide() over joint (agent, difficulty) labels when the
//      embedding pass abstains. The call starts before context loading and
//      shares its wait with that work. Low confidence still routes to the
//      best guess. Timeout, transport error, or no observed label routes
//      to the default agent.
//   3. heuristic_classify, only if the embedder did not return a decision
//      (runtime not linked, weights missing, or the call failed). A match
//      at IntentConfig::min_confidence returns without Luna.
//
// This path never calls the generative <intent> classifier.
//
// Policy numbers live in intent_route_defaults() and the env parser below.
// The router reads them from IntentRouteConfig; it does not embed cutoffs.
//
// Defaults (override with the matching ARBITER_INTENT_* variable):
//   live                  false          ARBITER_INTENT_LIVE
//   luna_min_confidence   0.55           ARBITER_INTENT_LUNA_MIN_CONFIDENCE
//   strong_difficulty     0.5            ARBITER_INTENT_STRONG_DIFFICULTY
//   default_difficulty    0.5            ARBITER_INTENT_DEFAULT_DIFFICULTY
//   timeout_ms            400            ARBITER_INTENT_LUNA_TIMEOUT_MS
//   cache_ttl_ms          60000          ARBITER_INTENT_CACHE_TTL_MS
//   cache_entries         64             ARBITER_INTENT_CACHE_ENTRIES
//   normalize_max_chars   512            ARBITER_INTENT_CACHE_KEY_CHARS
//   option_desc_chars     80             (prompt width; not a cutoff)
//   default_agent         "index"        ARBITER_INTENT_DEFAULT_AGENT
//   luna_model            gpt-6-luna     ARBITER_INTENT_LUNA_MODEL
//   cheap_model           empty          ARBITER_INTENT_CHEAP_MODEL
//   strong_model          empty          ARBITER_INTENT_STRONG_MODEL
//   embed_min_confidence  0.35           ARBITER_INTENT_EMBED_MIN_CONFIDENCE
//   embed_temperature     0.02           ARBITER_INTENT_EMBED_TEMPERATURE
//
// The heuristic cutoff is still IntentConfig::min_confidence (master
// default 0.8), and only applies when the embedding model is unavailable.
// Shadow mode (ARBITER_INTENT_SHADOW=luna or embed) is skipped while live
// mode already ran, so the same backend is not called twice.

#include "decision.h"
#include "intent.h"

#include <atomic>
#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arbiter {

struct IntentRouteConfig {
    bool live = false;
    double luna_min_confidence = 0.55;
    double strong_difficulty = 0.5;
    // Used when a successful call has no cheap/strong labels (roster wider
    // than the single-token budget). Not used for heuristic or fallback.
    double default_difficulty = 0.5;
    // Placeholder, not a measured Luna latency. A 2026-10-13 live smoke
    // (reasoning off, one token) was p50 1359 ms / p95 1564 ms over 4
    // calls; one-off curls were 4-5 s. At 400 ms nearly every live call
    // times out and falls back. Tune ARBITER_INTENT_LUNA_TIMEOUT_MS from
    // your own measurements before enabling live mode. Do not treat this
    // default as that measurement.
    int timeout_ms = 400;
    int cache_ttl_ms = 60000;     // 0 disables the cache
    int cache_entries = 64;
    int normalize_max_chars = 512;
    int option_desc_chars = 80;
    std::string default_agent = "index";
    std::string luna_model = kLunaModelId;
    std::string cheap_model;      // empty → the executing agent's model
    std::string strong_model;     // empty → advisor.model, else agent model
    // Embedding softmax confidence. Below this, live mode calls Luna.
    // The number is decision_confidence over the roster, not a raw cosine.
    double embed_min_confidence = 0.35;
    // Softmax temperature on cosine similarity. Does not change argmax;
    // it calibrates the probabilities the cutoff reads. 0.02 had the
    // lowest Brier and ECE on tests/data/intent_eval.jsonl.
    double embed_temperature = 0.02;
};

// Documented defaults. Not read from the environment.
IntentRouteConfig intent_route_defaults();

// defaults, then env overrides. Unparsable values keep the default.
// Confidences clamp to [0, 1]. timeout_ms and cache_entries clamp to ≥ 1.
// cache_ttl_ms clamps to ≥ 0.
IntentRouteConfig intent_route_config_from_env();

// ARBITER_INTENT_LIVE is luna / on / true / 1.
bool intent_live_luna_enabled();

struct IntentRouteTimings {
    // embed | heuristic | cache | luna | luna_guess | fallback | explicit
    std::string tier;
    bool cache_hit = false;
    double embed_ms = 0;
    double luna_ms = 0;
    double total_ms = 0;
    double difficulty = 0;
    bool difficulty_known = false;
    // cheap | strong | empty when this decision does not pick a model.
    std::string model_tier;
    bool choose_model = false;
    std::string error;
};

struct IntentRouteOutcome {
    Intent intent;
    IntentRouteTimings timings;
    // True when a Luna call was started (including timeout and error).
    bool used_luna = false;
};

// Honors cancel: return as soon as it becomes true. The router does not
// join a timed-out call on the request path.
using LunaDecideFn = std::function<DecisionResult(
    const DecisionRequest& request, const std::atomic<bool>& cancel)>;

// Runs on the caller, overlapping the Luna call. Speculative agent is the
// heuristic target when that is non-empty, otherwise the default agent.
using ContextLoadFn = std::function<void(const std::string& speculative_agent)>;

// Local embedding decide(). Empty means the model is not loaded; the
// router then keeps the heuristic. A low-confidence result skips the
// heuristic and falls through to Luna.
using EmbedDecideFn = std::function<DecisionResult(const DecisionRequest&)>;

// LRU of successful Luna decisions. Keyed by normalized text + roster ids.
// Timed-out workers are joined from reap() and the destructor, not from
// the request path.
class IntentRouteCache {
public:
    IntentRouteCache();
    ~IntentRouteCache();
    IntentRouteCache(const IntentRouteCache&) = delete;
    IntentRouteCache& operator=(const IntentRouteCache&) = delete;

    std::optional<DecisionResult> lookup(const std::string& key, int ttl_ms);
    void store(const std::string& key, const DecisionResult& result, int capacity);
    void adopt_worker(std::thread worker,
                      std::shared_ptr<std::atomic<bool>> cancel,
                      std::shared_ptr<std::atomic<bool>> finished);
    void reap();

private:
    struct Slot {
        DecisionResult result;
        std::chrono::steady_clock::time_point stored;
    };
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::shared_ptr<std::atomic<bool>> finished;
    };

    std::mutex mu_;
    std::list<std::string> lru_;
    std::unordered_map<std::string,
                       std::pair<Slot, std::list<std::string>::iterator>> map_;
    std::mutex workers_mu_;
    std::vector<Worker> workers_;
};

// Pure router. Does not call the generative intent classifier. Logs
// intent_route (tier, cache_hit, luna_ms, total_ms) on every return.
IntentRouteOutcome route_intent_live(const IntentInput& in,
                                     const IntentConfig& intent_cfg,
                                     const IntentRouteConfig& route_cfg,
                                     IntentRouteCache& cache,
                                     const LunaDecideFn& luna,
                                     const ContextLoadFn& context_load = {},
                                     const EmbedDecideFn& embed = {});

// True when a live-mode intent should rewrite index → target. embed, luna,
// and luna_guess apply below min_confidence. heuristic still uses that cutoff.
bool intent_route_should_apply(const IntentConfig& cfg,
                               const Intent& intent,
                               const std::string& requested_agent,
                               bool fresh_ingress = true);

// Empty when the chosen model is the agent's own model (no override).
// difficulty_known false uses route_cfg.default_difficulty.
std::string select_executor_model(const std::string& constitution_model,
                                  const std::string& advisor_model,
                                  double difficulty,
                                  bool difficulty_known,
                                  const IntentRouteConfig& cfg);

} // namespace arbiter
