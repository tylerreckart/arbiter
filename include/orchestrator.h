#pragma once
// index/include/orchestrator.h — Multi-agent orchestrator

#include "agent.h"
#include "api_client.h"
#include "commands.h"
#include <atomic>
#include <map>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>

namespace index_ai {

class Orchestrator {
public:
    explicit Orchestrator(std::map<std::string, std::string> api_keys);

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

    // Spawn a new UI pane running `agent_id` with `message` queued as its
    // first command.  The REPL provides this; without it (e.g. --send /
    // --serve callers) /pane commands from agents get an "ERR: pane
    // spawning unavailable" tool result and the agent is told not to retry.
    void set_pane_spawner(PaneSpawner cb) { pane_spawner_cb_ = std::move(cb); }

    // Intercept /write — when set, every /write in any turn (this agent or
    // a delegated one) routes through `cb` instead of touching the local
    // filesystem.  Used by the HTTP API to stream generated files to the
    // client as SSE events without persisting server-side.
    void set_write_interceptor(WriteInterceptor cb) { write_interceptor_cb_ = std::move(cb); }

    // Real-time read window into the tenant's structured memory.  When set,
    // /mem entries|entry|search resolve through this callback at every
    // turn and depth.  Without it, those subcommands return ERR.  The HTTP
    // API wires this against the request's authenticated tenant; CLI/REPL
    // contexts leave it null.
    void set_structured_memory_reader(StructuredMemoryReader cb) {
        structured_memory_reader_cb_ = std::move(cb);
    }

    // Write window for agent-contributed entries and links.  /mem add entry|link
    // routes through this callback and lands rows directly in the curated
    // graph — they surface to all readers (HTTP or agent) immediately.
    // Wired by the API per-tenant; CLI/REPL contexts leave it null.
    void set_structured_memory_writer(StructuredMemoryWriter cb) {
        structured_memory_writer_cb_ = std::move(cb);
    }

    // Bridge to the per-request MCP session manager.  /mcp tools|call
    // routes through this callback; the API server owns the
    // mcp::Manager that backs it (subprocesses die when the request's
    // orchestrator does).  Without this set, /mcp returns ERR — CLI/REPL
    // contexts don't spawn MCP servers.
    void set_mcp_invoker(MCPInvoker cb) { mcp_invoker_cb_ = std::move(cb); }

    // DB-backed file-scratchpad bridge.  /mem read|write|clear and
    // /mem shared read|write|clear route through this callback when set;
    // otherwise they fall back to the filesystem (~/.arbiter/memory/...).
    // The HTTP API wires this to a TenantStore-backed implementation so
    // tenant memory persists in the same database as conversations and
    // structured memory; the CLI/REPL leaves it null.
    void set_memory_scratchpad(MemoryScratchpadInvoker cb) {
        memory_scratchpad_cb_ = std::move(cb);
    }

    // Web-search bridge (/search <query> [top=N]).  Wired by the API
    // server to a Brave-backed (or other provider) HTTPS call when an
    // API key is configured; CLI/REPL contexts leave it null and the
    // dispatcher returns a clean ERR.
    void set_search_invoker(SearchInvoker cb) { search_invoker_cb_ = std::move(cb); }

    // Artifact-store bridges (/write --persist, /read, /list).  Bound
    // by the API server to TenantStore + the request's conversation_id
    // so artifacts are scoped to the active thread.  Without them set,
    // /write --persist degrades to ephemeral SSE only and /read / /list
    // return ERR.
    void set_artifact_writer(ArtifactWriter cb) { artifact_writer_cb_ = std::move(cb); }
    void set_artifact_reader(ArtifactReader cb) { artifact_reader_cb_ = std::move(cb); }
    void set_artifact_lister(ArtifactLister cb) { artifact_lister_cb_ = std::move(cb); }

    // Flip /exec off for this orchestrator.  Agents that emit /exec get a
    // tool result explaining the ban; they're expected to adapt their plan.
    // Used by the HTTP API so SaaS callers can't invoke arbitrary shell
    // commands on the server.
    void set_exec_disabled(bool v) { exec_disabled_ = v; }

    // ── Fleet-streaming callbacks ──────────────────────────────────────────
    //
    // An agent "turn" is one invocation of Agent::chat/stream; the master
    // call and every delegated /agent or /parallel child count as separate
    // turns.  Each turn is assigned a monotonically-increasing `stream_id`
    // (0 first, then 1, 2, ...) so a consumer can route text + tool + cost
    // events into the right UI slot even when parallel turns interleave on
    // the wire.  Callbacks here fire at every depth.
    //
    // `stream_start`: one shot before the turn's first API call.
    // `agent_stream`: every clean text delta produced during the turn
    //                 (after StreamFilter strips /cmd lines).
    // `stream_end`:   one shot after the turn finishes (ok=false on error).
    using StreamStartCallback = std::function<void(const std::string& agent_id,
                                                    int stream_id,
                                                    int depth)>;
    using AgentStreamCallback = std::function<void(const std::string& agent_id,
                                                    int stream_id,
                                                    const std::string& delta)>;
    using StreamEndCallback   = std::function<void(const std::string& agent_id,
                                                    int stream_id,
                                                    bool ok)>;
    void set_stream_start_callback(StreamStartCallback cb) { stream_start_cb_ = std::move(cb); }
    void set_agent_stream_callback(AgentStreamCallback  cb) { agent_stream_cb_ = std::move(cb); }
    void set_stream_end_callback  (StreamEndCallback    cb) { stream_end_cb_   = std::move(cb); }

    // Read the current thread's streaming context.  Callbacks (tool_status,
    // cost, progress, etc.) invoke these at emit time so every event carries
    // the `stream_id` and `agent` of whichever turn produced it — even when
    // /parallel has several turns running concurrently on different threads.
    // All three return defaults (0 / "" / 0) when called outside a turn.
    int                current_stream_id()    const;
    const std::string& current_stream_agent() const;
    int                current_stream_depth() const;

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

    // Fetch the Constitution for any loaded agent or the master "index".
    // Throws std::out_of_range if the id doesn't match.  Used by the HTTP
    // API's /v1/agents endpoints to surface agent metadata (role, goal,
    // advisor, capabilities) to clients.
    const Constitution& get_constitution(const std::string& id) const;

    // list_agents() but including the master "index" at the front.  Order
    // is master first, then children in insertion order.
    std::vector<std::string> list_agents_all() const;

    // Replace the agent's conversation history.  Used by the HTTP API's
    // /v1/conversations/:id/messages endpoint to resume a stored thread
    // before the next turn.  Works for "index" (master) and any loaded
    // sub-agent.  Throws std::out_of_range for unknown ids.
    void set_agent_history(const std::string& id, std::vector<Message> history);

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
    PaneSpawner        pane_spawner_cb_;
    WriteInterceptor   write_interceptor_cb_;
    StructuredMemoryReader structured_memory_reader_cb_;
    StructuredMemoryWriter structured_memory_writer_cb_;
    MCPInvoker         mcp_invoker_cb_;
    MemoryScratchpadInvoker memory_scratchpad_cb_;
    SearchInvoker      search_invoker_cb_;
    ArtifactWriter     artifact_writer_cb_;
    ArtifactReader     artifact_reader_cb_;
    ArtifactLister     artifact_lister_cb_;
    bool               exec_disabled_ = false;

    StreamStartCallback stream_start_cb_;
    AgentStreamCallback agent_stream_cb_;
    StreamEndCallback   stream_end_cb_;
    std::atomic<int>    stream_counter_{-1};   // next_stream_id returns 0 first
    int                 next_stream_id();

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

    // Dispatch-loop core, parameterised on the Agent instance.  Lets the
    // parallel invoker run two (or more) children with the same agent_id
    // by spinning up ephemeral Agent objects (cloned Constitution, fresh
    // history_) per child — without those clones, two threads would race
    // a single Agent's history_ vector.  agent_id is the *display* id
    // surfaced to callbacks and tool-result framing; the ephemeral Agent
    // need not be registered in agents_.
    ApiResponse run_dispatch(Agent& agent,
                              const std::string& agent_id,
                              const std::string& current_msg,
                              int depth,
                              std::map<std::string, std::string>* shared_cache,
                              const std::string& original_query);

    // Build an AgentInvoker lambda for use in command dispatch.
    // depth is the current nesting level; invoker refuses beyond depth 2.
    // shared_cache and original_query propagate through the delegation chain.
    AgentInvoker make_invoker(const std::string& caller_id, int depth,
                              std::map<std::string, std::string>* shared_cache,
                              const std::string& original_query);

    // Build a ParallelInvoker for /parallel fan-out.  Each child runs on its
    // own std::thread at depth+1 with a fresh dedup cache (shared caches
    // would be a data race on the std::map).  Rejects batches that would
    // race an Agent's history against itself (duplicate agent_ids).  All
    // threads join before the returned vector is filled — /parallel blocks
    // the calling turn until every child completes.
    ParallelInvoker make_parallel_invoker(const std::string& caller_id, int depth,
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
