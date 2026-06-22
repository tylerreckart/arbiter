// src/advisor.cpp — Framework-agnostic advisor-gate decision.
//
// The signal *parser* (parse_advisor_signal) lives in src/advisor_gate.cpp so
// it can be unit-tested without linking the provider client.  This translation
// unit adds the part that needs ApiClient: formatting the gate prompt and
// making the call.  Both the orchestrator's in-loop gate and the standalone
// POST /v1/advise/gate handler route through run_advisor_gate.

#include "advisor.h"
#include "api_client.h"
#include "commands.h"

#include <sstream>
#include <string>

namespace arbiter {

const char* default_gate_prompt() {
    // The prompt explicitly enumerates the three signals and the tag-based
    // grammar — the parser is strict about tag form, so the model must
    // produce it verbatim.  Kept identical to the wording shipped before the
    // gate was factored out of the orchestrator.
    return
        "You are a runtime gate evaluating whether an executor agent's "
        "terminating turn is acceptable to return to the caller.\n\n"
        "Inputs you receive (in this order):\n"
        "  - The original user task.\n"
        "  - The executor's outputs for the terminating turn (text only — "
        "no reasoning, no prior turns).\n"
        "  - A structured summary of tool calls made this turn.\n\n"
        "You will respond with EXACTLY ONE signal on its own line:\n\n"
        "  <signal>CONTINUE</signal>\n"
        "    The terminating turn satisfies the task; let the executor return.\n\n"
        "  <signal>REDIRECT</signal>\n"
        "  <guidance>...</guidance>\n"
        "    The executor is on the wrong track or stopped early.  Provide a "
        "concrete next step in <guidance>.  This will be injected as a "
        "synthetic user turn back to the executor.\n\n"
        "  <signal>HALT</signal>\n"
        "  <reason>...</reason>\n"
        "    The executor produced something the user must see before any "
        "further work — irreversible footgun about to commit, scope "
        "explosion, confidential data leak, fundamentally wrong premise. "
        "This will be surfaced to the user as an escalation.\n\n"
        "No preamble.  No markdown.  Output exactly one signal.  Default "
        "to CONTINUE when the turn is merely terse but correct.  Default "
        "to HALT when in doubt about safety; default to REDIRECT when in "
        "doubt about correctness.";
}

AdvisorGateOutput run_advisor_gate(
    ApiClient& client,
    const std::string& advisor_model,
    const std::string& prompt_override,
    const AdvisorGateInput& in,
    const std::function<void(const ApiResponse&)>& on_response) {

    AdvisorGateOutput out;

    if (advisor_model.empty()) {
        // Defence-in-depth: callers should already have checked
        // mode == "gate", but if a misconfiguration slips through we fail
        // closed with a HALT explaining the issue.
        out.kind = AdvisorGateOutput::Kind::Halt;
        out.text = "no advisor model configured for gate";
        out.malformed = true;
        return out;
    }

    std::ostringstream q;
    q << "[ORIGINAL TASK]\n" << in.original_task << "\n[END ORIGINAL TASK]\n\n"
      << "[EXECUTOR TERMINATING TURN]\n" << in.terminating_text
      << "\n[END EXECUTOR TERMINATING TURN]\n\n"
      << "[TOOL CALLS THIS TURN]\n"
      << (in.tool_summary.empty() ? "(none)\n" : in.tool_summary)
      << "[END TOOL CALLS]\n";

    ApiRequest req;
    req.model               = advisor_model;
    req.max_tokens          = 512;   // signals are short
    req.include_temperature = false; // reasoning models reject temperature
    req.system_prompt       = prompt_override.empty()
                              ? std::string(default_gate_prompt())
                              : prompt_override;
    req.messages            = {{"user", q.str()}};

    ApiResponse resp = client.complete(req);
    if (on_response) on_response(resp);
    if (!resp.ok) {
        out.kind = AdvisorGateOutput::Kind::Halt;
        out.text = "advisor API error: " + resp.error;
        out.malformed = true;
        out.raw  = resp.error;
        return out;
    }

    return parse_advisor_signal(resp.content);
}

} // namespace arbiter
