#pragma once
// index/include/orchestrator.h — Multi-agent orchestrator

#include "agent.h"
#include "api_client.h"
#include "commands.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>

namespace index_ai {

class Orchestrator {
public:
    explicit Orchestrator(const std::string& api_key);

    // Set directory used for agent memory files.  The REPL passes the
    // cwd-scoped path from get_memory_dir(); the default below is a harmless
    // fallback for callers (tests, --send one-shots) that forget to set it.
    void set_memory_dir(const std::string& dir) { memory_dir_ = dir; }

    // Optional callback fired after each sub-agent turn (depth > 0).
    // Called on the same thread as send()/send_streaming(), with any output
    // lock already held by the caller — do NOT re-acquire g_out_mu inside it.
    using ProgressCallback = std::function<void(const std::string& agent_id,
                                                 const std::string& content)>;
    void set_progress_callback(ProgressCallback cb);

    // Optional callback fired after every completed agent turn at any depth.
    // Use this to record sub-agent token costs that would otherwise be invisible
    // to a CostTracker wired only to the top-level REPL response handler.
    using CostCallback = std::function<void(const std::string& agent_id,
                                             const std::string& model,
                                             const ApiResponse& resp)>;
    void set_cost_callback(CostCallback cb);

    // Fired at the start of each sub-agent turn (before the API call).
    // Use to show a "working..." indicator in the UI.
    using AgentStartCallback = std::function<void(const std::string& agent_id)>;
    void set_agent_start_callback(AgentStartCallback cb);

    // Fired when an agent auto-compacts its context.  Wired to every managed
    // agent (master + existing + future) so newly-loaded agents also report.
    using CompactCallback = Agent::CompactCallback;
    void set_compact_callback(CompactCallback cb);

    // Gatekeeper for destructive agent actions — /write (always) and /exec
    // when the command matches a destructive pattern.  Called on the exec
    // thread; implementations must be thread-safe vs the main REPL thread.
    // Unset ⇒ all actions proceed without prompting.
    void set_confirm_callback(ConfirmFn cb) { confirm_cb_ = std::move(cb); }

    // Fired once per executed /cmd with (name, ok).  Wired by the REPL to
    // ToolCallIndicator so the spinner's count and ✓/✗ summary reflect real
    // post-exec status.  Fires for every tool call at any delegation depth —
    // main agent, sub-agent, sub-sub — so the turn's tally is unified.
    void set_tool_status_callback(ToolStatusFn cb) { tool_status_cb_ = std::move(cb); }

    // Agent management
    Agent& create_agent(const std::string& id, Constitution config);
    Agent& get_agent(const std::string& id);
    bool   has_agent(const std::string& id) const;
    void   remove_agent(const std::string& id);
    std::vector<std::string> list_agents() const;

    // Load agent definitions from directory
    void load_agents(const std::string& dir);

    // Send message to a specific agent.
    // Runs an agentic dispatch loop: if the agent's response contains
    // /fetch or /mem commands, they are executed and results fed back
    // automatically (up to 6 turns).
    ApiResponse send(const std::string& agent_id, const std::string& message);

    // Streaming variant of send() — streams first turn via callback,
    // falls back to non-streaming for tool-call re-entry turns.
    ApiResponse send_streaming(const std::string& agent_id,
                               const std::string& message,
                               StreamCallback cb);

    // Ask index (master) about system state — used by the TCP server.
    ApiResponse ask_index_ai(const std::string& query);

    // Return the model string for a given agent (or master if id == "index")
    std::string get_agent_model(const std::string& id) const;

    // Global stats
    std::string global_status() const;

    // Context compaction — summarize and clear one agent's history.
    // Returns the summary text, or "" if history was empty or the API call failed.
    // Works for "index" (master) and any loaded agent.
    std::string compact_agent(const std::string& agent_id);

    // ── Plan execution ──────────────────────────────────────────────────────────
    // Parse a plan markdown file (produced by the planner agent) and execute each
    // phase deterministically.  Phases run in dependency order; each phase's output
    // is injected into the task message of all phases that depend on it.
    //
    // progress_cb fires after each phase with a one-line status string.
    // Returns a report summarising what ran and what (if anything) failed.

    struct PlanPhase {
        int number = 0;
        std::string name;
        std::string agent;          // agent id to invoke
        std::vector<int> depends_on;
        std::string task;           // instruction passed to agent
        std::string output_desc;
        std::string acceptance;
    };

    struct PlanResult {
        bool ok = true;
        std::string error;
        // Ordered results per phase: (phase number, phase name, agent output)
        std::vector<std::tuple<int, std::string, std::string>> phases;
    };

    PlanResult execute_plan(const std::string& plan_path,
                            std::function<void(const std::string&)> progress = nullptr);

    // Session persistence — save/restore all agent conversation histories.
    // Histories are stored as JSON at the given path; agent configs come from
    // the normal .json files and are not duplicated in the session file.
    void save_session(const std::string& path) const;
    bool load_session(const std::string& path);  // returns true if anything loaded

    // Token tracking
    int total_input_tokens()  const { return client_.total_input_tokens(); }
    int total_output_tokens() const { return client_.total_output_tokens(); }

    ApiClient& client() { return client_; }

    // Interrupt any in-progress API call across the master and all agents.
    // Thread-safe — can be called from the readline/main thread while the
    // exec thread is blocked in a streaming read.
    void cancel();

private:
    ApiClient client_;
    std::unordered_map<std::string, std::unique_ptr<Agent>> agents_;
    mutable std::mutex agents_mutex_;
    std::string memory_dir_;
    ProgressCallback   progress_cb_;
    CostCallback       cost_cb_;
    AgentStartCallback start_cb_;
    CompactCallback    compact_cb_;
    ConfirmFn          confirm_cb_;
    ToolStatusFn       tool_status_cb_;

    // Master index agent for meta-queries
    std::unique_ptr<Agent> index_master_;

    // Core dispatch loop shared by send() and sub-agent invocations.
    // depth controls delegation nesting (max 2: index → agent → sub-agent).
    // shared_cache: cross-agent dedup cache (created at depth 0, propagated down).
    // original_query: user's original request (for sub-agent context injection).
    ApiResponse send_internal(const std::string& agent_id,
                              const std::string& message,
                              int depth = 0,
                              std::map<std::string, std::string>* shared_cache = nullptr,
                              const std::string& original_query = "");

    // Build an AgentInvoker lambda for use in command dispatch.
    // depth is the current nesting level; invoker refuses beyond depth 2.
    // shared_cache and original_query propagate through the delegation chain.
    AgentInvoker make_invoker(const std::string& caller_id, int depth,
                              std::map<std::string, std::string>* shared_cache,
                              const std::string& original_query);

    // Build an AdvisorInvoker bound to a specific caller.  Returns a lambda
    // that makes a one-shot, history-less call against the caller's
    // configured advisor_model (from the caller's Constitution).  If the
    // caller has no advisor_model set, the returned lambda returns an
    // ERR string explaining the misconfiguration.
    AdvisorInvoker make_advisor_invoker(const std::string& caller_id);

    // Truncation recovery.  If `cmds` contains an unclosed /write block
    // (body ended before /endwrite, typically because the model stopped
    // mid-file), ask the agent to resume at the exact cutoff and close
    // the block.  The continuation text is appended to `resp.content`
    // and `cmds` is re-parsed so callers see one complete /write instead
    // of persisting a half-written file.  Bounded retry count — if the
    // model can't close the block after a few tries, we give up and let
    // the caller execute whatever it has (with the truncation note).
    void recover_truncated_writes(Agent* agent,
                                  ApiResponse& resp,
                                  std::vector<AgentCommand>& cmds,
                                  StreamCallback cb);
};

} // namespace index_ai
