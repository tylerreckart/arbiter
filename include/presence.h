#pragma once
// arbiter/include/presence.h — Always-on peer observation + context injection.
//
// Presence is a constitution-declared residency class.  An always-on agent
// stays loaded and, at well-defined checkpoints (after a peer finishes a
// tool batch), reviews a snapshot of that peer's live work.  It may inject
// a CONTEXT note into the peer's next model turn, or stay SILENT.
//
// Job: pair over the shoulder — useful context for finishing the task
// at hand.  Not a second advisor: presence cannot CONTINUE/REDIRECT/HALT
// and must not grade or halt the turn.
//
// Distinct from:
//   • Advisor gate — terminating-turn CONTINUE/REDIRECT/HALT; fail-closed.
//     Presence cannot halt or redirect; it is fail-open additive context.
//   • LoopManager — a self-prompting thread of one agent.  Presence watches
//     *other* agents.
//   • Lessons / todos — static pre-turn injection.  Presence sees the live
//     tool summary of the current turn.
//   • JIT / reconcile — spawn-when-needed, teardown-when-done.  Presence is
//     the opposite lifetime: stay resident, observe, optionally interject.
//
// The decision function is history-less (one snapshot in, one signal out),
// matching advisor/intent.  Enforcement (injection into the tool-result
// envelope) is owned by the orchestrator.  Fail-open: transport or parse
// errors become SILENT so a watcher never blocks the working agent.

#include <functional>
#include <string>
#include <vector>

namespace arbiter {

class ApiClient;
struct ApiResponse;

struct PresenceConfig {
    // "off" (default) | "always_on"
    std::string mode = "off";
    // Agent-id globs (fnmatch).  Empty → watch every peer (not self).
    std::vector<std::string> watch;
    // "off" | "context" (default).  "off" declares residency but skips
    // the model call and injection — useful as a kill switch.
    std::string interject = "context";
    // Optional model override.  Empty → watcher's Constitution::model.
    std::string model;
    // Optional review-prompt override.  Empty → default_presence_prompt().
    std::string prompt;
    // Cap on CONTEXT notes this watcher may inject per working-agent
    // stream_id.  Clamped to [1, 4] at parse time.
    int max_notes_per_turn = 1;
};

inline bool presence_is_active(const PresenceConfig& cfg) {
    return cfg.mode == "always_on" && cfg.interject != "off";
}

inline std::string presence_model(const PresenceConfig& cfg,
                                  const std::string& fallback_model) {
    return cfg.model.empty() ? fallback_model : cfg.model;
}

// True when this watcher should review `agent_id`.  Empty watch ≡ "*".
// Never matches the empty string.
bool presence_watch_matches(const PresenceConfig& cfg,
                            const std::string& agent_id);

struct PresenceInput {
    std::string watcher_id;
    std::string watcher_name;
    std::string watcher_goal;
    std::string watcher_rules;       // joined "- rule" lines
    std::string working_agent_id;
    std::string working_agent_role;
    std::string original_task;
    std::string recent_text;         // executor's last assistant turn
    std::string tool_summary;        // same shape as advisor tool_summary
};

struct PresenceOutput {
    enum class Kind { Silent, Context };
    Kind        kind = Kind::Silent;
    std::string text;                // CONTEXT note body
    std::string raw;
    bool        malformed = false;
};

// Field caps — same order of magnitude as the advisor gate.  Presence
// reviews mid-turn, so we keep the snapshot small on purpose.
inline constexpr size_t kPresenceMaxOriginalTask   = 16 * 1024;
inline constexpr size_t kPresenceMaxRecentText     = 16 * 1024;
inline constexpr size_t kPresenceMaxToolSummary    = 8 * 1024;
inline constexpr size_t kPresenceMaxPromptOverride = 8 * 1024;
inline constexpr size_t kPresenceMaxNote           = 2 * 1024;
inline constexpr int    kMaxPresenceWatchersPerCheckpoint = 3;

void cap_presence_input(PresenceInput& in);
std::string cap_presence_prompt_override(const std::string& prompt);

const char* default_presence_prompt();

// Parse a model reply into SILENT / CONTEXT.  Missing <signal> or an
// unknown token → Silent + malformed.  CONTEXT without <note> is
// malformed Silent (fail-open).  Surrounding prose is tolerated.
PresenceOutput parse_presence_signal(const std::string& reply);

// `[PRESENCE: Name]\n<body>\n[END PRESENCE]\n\n` — prepended to the
// working agent's next user-role tool-result envelope.
std::string format_presence_injection(const std::string& watcher_name,
                                      const std::string& note);

// History-less review call.  Empty model or transport error → Silent
// (fail-open).  on_response fires with the raw ApiResponse for cost
// attribution, matching run_advisor_gate.
PresenceOutput run_presence_review(
    ApiClient& client,
    const std::string& model,
    const std::string& prompt_override,
    const PresenceInput& in,
    const std::function<void(const ApiResponse&)>& on_response = nullptr);

} // namespace arbiter
