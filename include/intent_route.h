#pragma once
// arbiter/include/intent_route.h — Latency-first ingress routing.
//
// Live mode (ARBITER_INTENT_LIVE=luna, default off) replaces the full-LLM
// intent fallback. The request path returns as soon as a local decision
// exists. Luna, when enabled, runs after that return.
//
//   1. A fresh cached Luna decision for the normalized request is the
//      route. That is how the next similar request benefits from a
//      background call. A hit does not start another Luna call.
//   2. Otherwise the local embedding router decides. Its choice is the
//      route at every confidence. High confidence (embed_min_confidence)
//      leaves Luna idle. Low confidence still returns the embedding
//      choice and starts Luna in the background.
//   3. When the embedding model is missing or the call fails, the route
//      is the default agent. Luna still starts in the background so a
//      later similar request can hit the cache.
//
// The heuristic is not on this path. This path never calls the generative
// <intent> classifier.
//
// Background Luna uses joint (agent, difficulty) labels. A successful
// result is stored in the decision cache and written beside the embedding
// choice in the decision log. Timeout, transport error, or no observed
// label is left out of the cache. The timeout is a budget for that
// background call. Routing does not wait on it.
//
// ARBITER_INTENT_LUNA_REROUTE (default off) may swap the specialist if
// Luna has already finished before the turn's first model call, the
// agents differ, and Luna's confidence is at least luna_min_confidence.
// The swap leaves the executor model alone. The stream's agent id stays
// the synchronous choice; only the agent instance that receives the
// prompt changes.
//
// Policy numbers live in intent_route_defaults() and the env parser below.
// The router reads them from IntentRouteConfig; it does not embed cutoffs.
//
// Defaults (override with the matching ARBITER_INTENT_* variable):
//   live                  false          ARBITER_INTENT_LIVE
//   luna_min_confidence   0.55           ARBITER_INTENT_LUNA_MIN_CONFIDENCE
//   strong_difficulty     0.5            ARBITER_INTENT_STRONG_DIFFICULTY
//   default_difficulty    0.5            ARBITER_INTENT_DEFAULT_DIFFICULTY
//   timeout_ms            2000           ARBITER_INTENT_LUNA_TIMEOUT_MS
//   luna_reroute          false          ARBITER_INTENT_LUNA_REROUTE
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
// Shadow mode (ARBITER_INTENT_SHADOW=luna or embed) is skipped while live
// mode already ran, so the same backend is not called twice.

#include "decision.h"
#include "intent.h"

#include <atomic>
#include <chrono>
#include <deque>
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
    // Budget for the background Luna call. Routing returns before this
    // elapses. Sized from live OpenRouter measurements of openai/gpt-6-luna
    // with reasoning off: about 1.0–1.4 s at p50 and 1.6–1.9 s at p95.
    // A 2026-10-13 smoke over 4 calls was p50 1359 ms / p95 1564 ms.
    // The previous 400 ms default expired before those calls finished.
    int timeout_ms = 2000;
    // When true, a finished high-confidence Luna disagreement may replace
    // the specialist before the first model call. Default off.
    bool luna_reroute = false;
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

// One ingress decision, for later calibration. Luna fields stay empty
// until the background call settles. advisor_verdict stays empty until
// note_intent_advisor records continue / redirect / halt.
struct IntentDecisionRecord {
    std::string request_hash;
    std::string embed_choice;
    double embed_prob = 0;
    bool embed_prob_known = false;
    std::string luna_choice;
    double luna_prob = 0;
    bool luna_prob_known = false;
    double route_ms = 0;
    double luna_ms = 0;
    std::string final_agent;
    std::string advisor_verdict;
    std::string tier;
};

// Cheap specialist swap. apply is true only when luna_reroute is on,
// Luna has already finished, the agent differs from the synchronous
// choice, and confidence is at least luna_min_confidence.
struct LunaReroute {
    bool apply = false;
    std::string agent;
    double confidence = 0;
    std::string source;
};

struct IntentRouteTimings {
    // embed | cache | luna | luna_guess | fallback | explicit
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
    // True when a background Luna call was started.
    bool used_luna = false;
    // SHA-256 of the cache key. Empty when no record was written.
    std::string request_hash;
    // Set when a background call was started. Tests wait on it; the
    // request path does not.
    std::shared_ptr<std::atomic<bool>> luna_finished;
};

// Honors cancel: return as soon as it becomes true. The router does not
// join the call on the request path. A watcher inside the worker sets
// cancel when timeout_ms elapses.
using LunaDecideFn = std::function<DecisionResult(
    const DecisionRequest& request, const std::atomic<bool>& cancel)>;

// Runs on the caller while the background Luna call is in flight.
// Speculative agent is the synchronous route (embedding choice, else
// the default agent).
using ContextLoadFn = std::function<void(const std::string& speculative_agent)>;

// Local embedding decide(). Empty, or a failed result, routes to the
// default agent and may start background Luna.
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

    // In-memory ring (newest first, 128 entries) of sync decisions.
    // complete_luna fills the Luna columns when the background call ends.
    void record_decision(IntentDecisionRecord rec);
    void set_route_ms(const std::string& request_hash, double route_ms);
    void note_advisor(const std::string& request_hash, const std::string& verdict);
    std::optional<IntentDecisionRecord> find_decision(const std::string& request_hash);
    std::vector<IntentDecisionRecord> decisions();

    // apply is false when the flag is off, Luna is still running, the
    // agents agree, or confidence is below luna_min_confidence.
    LunaReroute poll_reroute(const std::string& request_hash,
                             const IntentRouteConfig& cfg);

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
    struct LunaWatch {
        std::string sync_agent;
        std::shared_ptr<std::atomic<bool>> finished;
        bool settled = false;
        bool ok = false;
        std::string agent;
        double probability = 0;
        bool probability_known = false;
        double confidence = 0;
        bool consumed = false;
    };

    void store_unlocked(const std::string& key, const DecisionResult& result, int capacity);
    void track_luna(const std::string& request_hash,
                    const std::string& sync_agent,
                    const std::shared_ptr<std::atomic<bool>>& finished);
    void settle_luna(const std::string& request_hash,
                     const std::string& key,
                     const DecisionResult* store,
                     int capacity,
                     const std::string& luna_choice,
                     double luna_prob,
                     bool luna_prob_known,
                     double luna_ms,
                     double confidence);

    friend IntentRouteOutcome route_intent_live(const IntentInput& in,
                                                const IntentConfig& intent_cfg,
                                                const IntentRouteConfig& route_cfg,
                                                IntentRouteCache& cache,
                                                const LunaDecideFn& luna,
                                                const ContextLoadFn& context_load,
                                                const EmbedDecideFn& embed);

    mutable std::mutex mu_;
    std::list<std::string> lru_;
    std::unordered_map<std::string,
                       std::pair<Slot, std::list<std::string>::iterator>> map_;
    std::mutex workers_mu_;
    std::vector<Worker> workers_;
    std::deque<IntentDecisionRecord> decisions_;
    std::list<std::string> watch_order_;
    std::unordered_map<std::string, LunaWatch> watches_;
};

// Pure router. Does not call the generative intent classifier. Logs
// intent_route on every return and intent_decision for the calibration row.
// Returns the synchronous route. A background Luna call, when started, is
// adopted by the cache and joined from reap() or the destructor.
IntentRouteOutcome route_intent_live(const IntentInput& in,
                                     const IntentConfig& intent_cfg,
                                     const IntentRouteConfig& route_cfg,
                                     IntentRouteCache& cache,
                                     const LunaDecideFn& luna,
                                     const ContextLoadFn& context_load = {},
                                     const EmbedDecideFn& embed = {});

// Thread-local turn binding so dispatch can poll a reroute and the advisor
// gate can attach a verdict without threading the hash through every caller.
void intent_route_bind_turn(IntentRouteCache* cache, const std::string& request_hash);
void intent_route_clear_turn();
LunaReroute poll_luna_reroute_for_turn(const IntentRouteConfig& cfg);
void note_intent_advisor(IntentRouteCache& cache,
                         const std::string& request_hash,
                         const std::string& verdict);
void note_intent_advisor_for_turn(const std::string& verdict);

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
