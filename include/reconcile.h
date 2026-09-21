#pragma once
// arbiter/include/reconcile.h — Intent reconcile (desired end state).
//
// Compiles `target_state` + `invariants` into a versioned state contract,
// observes a bound workspace, requires verification (tests), and optionally
// snapshots/restores on failure.  Phase A (#209) shipped admit + observe +
// verify + rollback.  Phase B (#208) turns mode=ensure into a JIT ΔS wave
// loop: resolve covering agents, spawn ephemeral clones, re-observe, teardown.
//
// What this module does NOT do: rewrite POST /v1/orchestrate, persist a
// materialized S_current table, or use advisor CONTINUE/REDIRECT/HALT as
// the success supervisor.  Success is runtime re-observe (ΔS empty) plus
// required verification.  An optional implement hook lets tests stub a
// wave without a live LLM; HTTP ensure wires an orchestrator ephemeral
// invoker (fleet shim) for real agent turns.
//
// Distinct from resolve_intent() (utterance classify/route).  Reconcile
// takes a typed desired state, not a chat prompt.

#include "json.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace arbiter {

// Wall-clock cap for reconcile verification test runs (host and sandbox).
constexpr int kReconcileVerifyTimeoutSec = 120;

// Closed checker names.  Unknown checkers fail closed (admit on extra
// clauses; observe residual with detail "unknown checker").
bool reconcile_checker_known(const std::string& checker);

struct ReconcileInvariant {
    enum class Tier { Expr, Named };
    Tier        tier = Tier::Named;
    std::string raw;
    std::string field;   // expr
    std::string op;      // <= >= < > == !=
    std::string value;   // number or unquoted string
};

struct StateClause {
    std::string id;
    std::string checker;  // expr.holds | named.capability | file.exists
                          // | verification.pass | workspace.mentions
                          // | todo.completed | artifact.exists | memory.active
    std::string arg;
    std::string agent;    // agent_map / capability fallback (Phase B JIT)
};

struct StateContract {
    std::string id;
    int         version = 1;
    std::vector<StateClause> clauses;
    int         max_waves = 12;
    int64_t     max_wall_ms = 1'800'000;
    int         max_agents_per_wave = 8;
    int         max_retries_per_clause = 2;
};

struct ClauseResult {
    std::string id;
    std::string checker;
    bool        satisfied = false;
    std::string detail;
};

struct DeltaS {
    std::vector<ClauseResult> residual;
    std::vector<ClauseResult> held;
    bool empty() const { return residual.empty(); }
};

struct ReconcileWorkspace {
    std::string kind;  // "sandbox" | "path"
    std::string root;  // resolved absolute directory
};

struct ReconcileVerification {
    bool        require_tests = true;
    std::string command = "auto";  // "auto" or an explicit runner
};

struct ReconcileSpec {
    std::string              target_state_json;  // JSON object
    std::vector<std::string> invariants;
    ReconcileWorkspace       workspace;
    ReconcileVerification    verification;
    bool                     rollback_on_failure = false;
    std::string              mode = "observe";  // observe | ensure
    int                      max_waves = 12;
    int64_t                  max_wall_ms = 1'800'000;
    int                      max_agents_per_wave = 8;
    int                      max_retries_per_clause = 2;
    // clause id → agent id.  Present keys override compile defaults and
    // capability fallback (empty value = explicitly unmapped).
    std::map<std::string, std::string> agent_map;
    // Optional extra clauses (tests / explicit contract slices).  Appended
    // after compiled target_state + invariant clauses.
    std::vector<StateClause> extra_clauses;
    std::string              snapshot_dir;  // empty → sibling .arbiter-reconcile-snapshots
};

struct AdmitError {
    std::string code;
    std::string message;
};

struct VerificationEvidence {
    bool        ran = false;
    bool        passed = false;
    std::string command;
    int         exit_code = -1;
    std::string log;     // truncated
    std::string reason;  // missing | undetectable | skipped | ...
};

struct ReconcileResult {
    std::string              status;  // satisfied | failed | rolled_back | canceled
    std::string              reason;
    StateContract            contract;
    DeltaS                   delta;
    VerificationEvidence     verification;
    std::vector<std::string> files_changed;
    bool                     rolled_back = false;
    std::string              brief;
    std::string              snapshot_path;
    int                      waves = 0;
};

// One covering agent for a slice of residual ΔS in a wave.
struct ReconcileAgentCover {
    std::string              agent_id;
    std::string              clone_id;
    std::vector<std::string> clause_ids;
    std::string              prompt;
};

struct ReconcileAgentTurn {
    std::string agent_id;
    std::string clone_id;
    bool        ok = true;
    std::string note;
};

struct ReconcileHooks {
    // Optional per-wave workspace mutator (test stub).  Invoked once per
    // ensure wave when `agent_turn` is unset.  HTTP ensure wires
    // `agent_turn` to orchestrator ephemeral clones instead.
    using ImplementFn = std::function<std::string(const StateContract&,
                                                  const std::string& workspace_root,
                                                  std::atomic<bool>* cancel)>;
    // Optional sandbox verification runner.  Required when
    // workspace.kind=sandbox so tests execute inside the tenant
    // container instead of on the API host.
    using VerifyExecFn = std::function<VerificationEvidence(
        const std::string& command, std::atomic<bool>* cancel)>;
    // Per-cover ephemeral agent turn.  Must operate on a clone, never
    // mutate canonical catalog agent history.  Stack-local clones are
    // torn down when this returns (or throws).
    using AgentTurnFn = std::function<ReconcileAgentTurn(
        const ReconcileAgentCover& cover,
        const StateContract& contract,
        const std::string& workspace_root,
        std::atomic<bool>* cancel)>;
    // Live SSE/progress sink.  Used for reconcile.delta, agent.spawned,
    // agent.teardown during the wave loop.
    using EventFn = std::function<void(const std::string& event,
                                       const std::shared_ptr<JsonValue>& payload)>;
    using AgentKnownFn = std::function<bool(const std::string& agent_id)>;
    using TodoCompletedFn = std::function<bool(const std::string& subject)>;
    using ArtifactExistsFn = std::function<bool(const std::string& path)>;
    using MemoryActiveFn = std::function<bool(const std::string& type,
                                              const std::string& tag)>;

    ImplementFn      implement;
    VerifyExecFn     verify_exec;
    AgentTurnFn      agent_turn;
    EventFn          emit;
    AgentKnownFn     agent_known;
    TodoCompletedFn  todo_completed;
    ArtifactExistsFn artifact_exists;
    MemoryActiveFn   memory_active;
    std::atomic<bool>* cancel = nullptr;
};

// Closed named-invariant catalog.  Unknown names fail admit (not ignore).
bool named_invariant_known(const std::string& name);
const std::vector<std::string>& named_invariant_catalog();

std::optional<ReconcileInvariant>
parse_invariant(const std::string& raw, std::string* err);

// Fail-closed admit: unknown names, bad expr, contradictory target_state,
// empty/invalid workspace, unsupported mode, unknown extra checkers,
// out-of-range budgets.
std::optional<AdmitError> admit_reconcile(const ReconcileSpec& spec);

StateContract compile_intent_contract(const ReconcileSpec& spec,
                                      const std::string& contract_id);

DeltaS observe_contract(const StateContract& contract,
                        const ReconcileSpec& spec,
                        const ReconcileHooks& hooks = {});

// Capability fallback when neither agent_map nor StateClause.agent is set.
std::string capability_fallback_agent(const std::string& checker);

// Resolve covering agent for a clause: explicit agent_map (incl. empty),
// else clause.agent, else capability fallback.
std::string resolve_clause_agent(const StateClause& clause,
                                 const ReconcileSpec& spec);

// Group residual implementation clauses into ≤ max_agents covers.
std::vector<ReconcileAgentCover>
resolve_wave_covers(const DeltaS& delta,
                    const StateContract& contract,
                    const ReconcileSpec& spec,
                    int wave_index);

// Detect npm/pytest/cargo/go/ctest/make test.  Empty if undetectable.
std::string detect_test_command(const std::string& workspace_root);

// Reject unsafe shell.  Empty command + require_tests → missing.
bool verification_command_is_safe(const std::string& command);

VerificationEvidence run_verification(const ReconcileSpec& spec,
                                      const ReconcileHooks& hooks = {});

bool snapshot_workspace(const std::string& root,
                        const std::string& dest,
                        std::string* err);
// Copy `snapshot` into `root`.  The snapshot is staged first so a
// failed copy cannot clear the live tree; only a successful stage
// replaces workspace contents (`.arbiter-reconcile-snapshots` is kept).
bool restore_workspace(const std::string& snapshot,
                       const std::string& root,
                       std::string* err);

ReconcileResult run_reconcile(const ReconcileSpec& spec,
                              const ReconcileHooks& hooks = {});

std::string format_reconcile_brief(const StateContract& c,
                                   const ReconcileSpec& spec);

std::shared_ptr<JsonValue> contract_to_json(const StateContract& c);
std::shared_ptr<JsonValue> result_to_json(const ReconcileResult& r);
std::shared_ptr<JsonValue> delta_to_json(const DeltaS& d);
std::shared_ptr<JsonValue> agent_cover_to_json(const ReconcileAgentCover& c);

}  // namespace arbiter
