#pragma once
// arbiter/include/reconcile.h — Intent reconcile (desired end state).
//
// Compiles `target_state` + `invariants` into a versioned state contract,
// observes a bound workspace, requires verification (tests), and optionally
// snapshots/restores on failure.  This is the Phase A facade for GitHub
// #209 (product/SDK) over the #208 contract substrate.
//
// What this module does NOT do: JIT-spawn specialists, persist todos, or
// replace /v1/orchestrate.  An optional implement hook lets tests (and a
// later fleet shim) mutate the tree; HTTP observe-mode returns residual
// ΔS when the workspace is not yet satisfied.
//
// Distinct from resolve_intent() (utterance classify/route).  Reconcile
// takes a typed desired state, not a chat prompt.

#include "json.h"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace arbiter {

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
    std::string arg;
    std::string agent;    // optional agent_map hint (Phase B JIT)
};

struct StateContract {
    std::string id;
    int         version = 1;
    std::vector<StateClause> clauses;
    int         max_waves = 12;
    int64_t     max_wall_ms = 1'800'000;
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
};

struct ReconcileHooks {
    // Optional workspace mutator.  Return a short note (logged as
    // reconcile.progress).  Tests inject a stub that writes files;
    // HTTP ensure with no hook leaves residual ΔS as implement_required.
    using ImplementFn = std::function<std::string(const StateContract&,
                                                  const std::string& workspace_root,
                                                  std::atomic<bool>* cancel)>;
    ImplementFn      implement;
    std::atomic<bool>* cancel = nullptr;
};

// Closed named-invariant catalog.  Unknown names fail admit (not ignore).
bool named_invariant_known(const std::string& name);
const std::vector<std::string>& named_invariant_catalog();

std::optional<ReconcileInvariant>
parse_invariant(const std::string& raw, std::string* err);

// Fail-closed admit: unknown names, bad expr, contradictory target_state,
// empty/invalid workspace, unsupported mode.
std::optional<AdmitError> admit_reconcile(const ReconcileSpec& spec);

StateContract compile_intent_contract(const ReconcileSpec& spec,
                                      const std::string& contract_id);

DeltaS observe_contract(const StateContract& contract,
                        const ReconcileSpec& spec);

// Detect npm/pytest/cargo/go/ctest/make test.  Empty if undetectable.
std::string detect_test_command(const std::string& workspace_root);

// Reject unsafe shell.  Empty command + require_tests → missing.
bool verification_command_is_safe(const std::string& command);

VerificationEvidence run_verification(const ReconcileSpec& spec,
                                      std::atomic<bool>* cancel = nullptr);

bool snapshot_workspace(const std::string& root,
                        const std::string& dest,
                        std::string* err);
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

}  // namespace arbiter
