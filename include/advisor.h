#pragma once
// arbiter/include/advisor.h — Framework-agnostic advisor-gate decision.
//
// The advisor gate's *decision* — "is this terminating turn acceptable to
// return to the caller?" — is a pure, stateless function: one history-less
// model call in, one CONTINUE/REDIRECT/HALT signal out.  It is deliberately
// split from the orchestrator so two callers can share one implementation:
//
//   • Orchestrator::make_advisor_gate_invoker — the in-loop gate that fires
//     on an executor's terminating turn (src/orchestrator.cpp).
//   • The HTTP handler for POST /v1/advise/gate — exposes the same decision
//     to external frameworks that own their own executor loop.
//
// What does NOT live here, by design: the *enforcement* ("the executor
// cannot return without CONTINUE"), the redirect budget, and synthetic-turn
// re-injection.  Those are loop-control concerns owned by whoever drives the
// executor.  Over HTTP the calling framework must honour the verdict itself —
// the unbypassability guarantee is a property of owning the loop, not of this
// function.  See docs/concepts/advisor.md.

#include "commands.h"   // AdvisorGateInput / AdvisorGateOutput / parse_advisor_signal

#include <functional>
#include <string>

namespace arbiter {

class ApiClient;
struct ApiResponse;

// The runtime gate's default system prompt.  Exposed so the in-loop gate and
// the standalone endpoint share one source of truth; callers override it via
// the `prompt_override` argument (constitution `advisor.prompt`, or the
// request body's `advisor.prompt`).
const char* default_gate_prompt();

// Make one history-less advisor call and parse its reply into a gate signal.
//
//   advisor_model    provider-prefixed model id (e.g. "claude-opus-4-7").
//                    Empty ⇒ returns Halt(malformed) — defence-in-depth; the
//                    caller is expected to have checked mode == "gate" first.
//   prompt_override  gate system-prompt override; empty ⇒ default_gate_prompt().
//   in               structured executor context (task, terminating text,
//                    tool summary).
//   on_response      optional hook fired with the raw ApiResponse after the
//                    call — used by the orchestrator to attribute cost to the
//                    caller's ledger.  A standalone deployment wires its own
//                    metering here (the runtime keeps no usage ledger).
//
// On transport/model error returns kind=Halt, malformed=true, with the
// provider error in `text`/`raw`.  This function applies NO fail-open /
// fail-closed policy on a *parseable-but-malformed* reply — it returns the
// parser's verdict (which may be kind=Continue with malformed=true) and lets
// the caller apply its own `malformed_halts` policy, exactly as the in-loop
// gate does.
AdvisorGateOutput run_advisor_gate(
    ApiClient& client,
    const std::string& advisor_model,
    const std::string& prompt_override,
    const AdvisorGateInput& in,
    const std::function<void(const ApiResponse&)>& on_response = nullptr);

} // namespace arbiter
