#pragma once
// arbiter/include/intent.h — Pre-dispatch intent classification.
//
// Pure, stateless classify/route: utterance or event text in, an Intent
// out.  Deliberately split from the orchestrator so two callers share one
// implementation:
//
//   • Orchestrator depth-0 ingress — may short-circuit index → specialist.
//   • POST /v1/intent — standalone classify for external loops.
//
// What does NOT live here: writ dispatch, todo persistence, plan execution.
// Seed slots are populated for Phase 5 consumers; this module never writes
// them.  Fail-open: transport/parse errors return kind=unknown with an empty
// target so today's index + writ loop continues unchanged.
//
// Distinct from Constitution::MemoryConfig::intent_routing, which only
// boosts /mem search types.

#include <functional>
#include <string>
#include <vector>

namespace arbiter {

struct IntentConfig {
    // off | heuristic | hybrid | llm
    // File-backed agents default off; master_constitution() sets hybrid.
    std::string mode = "off";
    double      min_confidence = 0.8;
    std::string model;              // empty → caller supplies advisor.model
    bool        apply_routing = true;
};

struct IntentRosterEntry {
    std::string id;
    std::string role;
    std::string goal;
    std::vector<std::string> capabilities;
};

struct IntentSeedTodo {
    std::string title;
    std::string detail;
};

struct IntentSeedPhase {
    std::string name;
    std::string agent;
    std::string task;
};

struct Intent {
    // Closed taxonomy aligned with starter agents, plus multi/unknown.
    std::string kind;            // research|review|write|ops|frontend|backend|plan|market|social|multi|unknown
    double      confidence = 0;  // 0..1
    std::string source;          // heuristic|llm|explicit|event|none
    std::string target_agent;    // empty = do not reroute
    std::string brief;
    std::vector<IntentSeedTodo>  todo_seeds;
    std::vector<IntentSeedPhase> plan_seeds;
    bool llm_used = false;
    bool malformed = false;
};

struct IntentInput {
    std::string text;              // original_query if set, else user/event text
    std::string requested_agent;   // send() agent_id; empty treated as index
    std::vector<IntentRosterEntry> roster;
    std::string source_hint;       // "event" from POST /v1/events; else empty
};

// Optional LLM: return the model's raw reply (tagged <intent>…), or empty
// on transport/model error.  resolve_intent treats empty as fail-open.
using IntentLlmFn = std::function<std::string(const std::string& user_prompt)>;

const char* default_intent_prompt();

bool intent_kind_is_valid(const std::string& kind);

// Parse a model reply into an Intent.  Unknown/missing <kind> ⇒ malformed.
// <agent> is copied verbatim; callers drop ids that are not on the roster.
Intent parse_intent_signal(const std::string& reply);

// Heuristic-only classify (no LLM).  Used by resolve_intent and tests.
Intent heuristic_classify(const IntentInput& in);

// True when orch/API should rewrite the ingress agent to intent.target_agent.
// `fresh_ingress` is false when original_query was set (loop / HTTP
// continuation) — classify may still run; routing must not.
bool intent_should_apply(const IntentConfig& cfg,
                         const Intent& intent,
                         const std::string& requested_agent,
                         bool fresh_ingress = true);

// Hybrid classify: explicit specialist short-circuits; heuristic when
// confident; LLM when mode is hybrid/llm and heuristic is below threshold.
// llm may be null — hybrid then degrades to heuristic-only.
Intent resolve_intent(const IntentInput& in,
                      const IntentConfig& cfg,
                      const IntentLlmFn& llm = nullptr);

// One-line (plus optional GOAL) preamble injected ahead of the user text.
// Empty when there is nothing useful to show the executor.
std::string format_intent_preamble(const Intent& intent, bool for_specialist);

} // namespace arbiter
