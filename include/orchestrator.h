#pragma once
// arbiter/include/orchestrator.h — Multi-agent orchestrator

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

namespace arbiter {

// Scan *.json agent files in agents_dir for the first agent whose event_types
// array contains a glob pattern matching event_type.  Returns that agent's id
// (Constitution::name or filename stem), or "index" if no match.  Reads
// constitution files on each call — intended for the infrequent /v1/events
// dispatch path, not a hot loop.
std::string route_event(const std::string& agents_dir,
                        const std::string& event_type);

class Orchestrator {
public:
    explicit Orchestrator(std::map<std::string, std::string> api_keys);

    void set_memory_dir(const std::string& dir) { memory_dir_ = dir; }

    // Fired after each sub-agent turn (depth > 0).
    using ProgressCallback = std::function<void(const std::string& agent_id,
                                                 const std::string& content)>;
    void set_progress_callback(ProgressCallback cb);

    // Fired after every completed turn at any depth (includes sub-agents).
    using CostCallback = std::function<void(const std::string& agent_id,
                                             const std::string& model,
                                             const ApiResponse& resp)>;
    void set_cost_callback(CostCallback cb);

    // Fired at the start of each turn (before the API call).
    using AgentStartCallback = std::function<void(const std::string& agent_id)>;
    void set_agent_start_callback(AgentStartCallback cb);

    // Gate for destructive actions (/write, /exec). Unset ⇒ proceed without prompting.
    // Must be thread-safe vs the REPL thread.
    void set_confirm_callback(ConfirmFn cb) { confirm_cb_ = std::move(cb); }

    // Fired once per /cmd with (name, ok), at every delegation depth.
    void set_tool_status_callback(ToolStatusFn cb) { tool_status_cb_ = std::move(cb); }

    // Spawn a UI pane. Without it, /pane returns ERR.
    void set_pane_spawner(PaneSpawner cb) { pane_spawner_cb_ = std::move(cb); }

    // Intercept /write; without it, files land on the local filesystem.
    void set_write_interceptor(WriteInterceptor cb) { write_interceptor_cb_ = std::move(cb); }

    // /mem entries|entry|search bridge. Without it, those subcommands return ERR.
    void set_structured_memory_reader(StructuredMemoryReader cb) {
        structured_memory_reader_cb_ = std::move(cb);
    }

    // /mem add entry|link bridge. Without it, returns ERR.
    void set_structured_memory_writer(StructuredMemoryWriter cb) {
        structured_memory_writer_cb_ = std::move(cb);
    }

    // /mcp tools|call bridge. Without it, returns ERR.
    void set_mcp_invoker(MCPInvoker cb) { mcp_invoker_cb_ = std::move(cb); }

    // /a2a list|card|call bridge. Without it, returns ERR.
    void set_a2a_invoker(A2AInvoker cb) { a2a_invoker_cb_ = std::move(cb); }

    // /schedule bridge. Without it, returns ERR.
    void set_scheduler_invoker(SchedulerInvoker cb) {
        scheduler_invoker_cb_ = std::move(cb);
    }

    // /todo bridge; open todos are injected into sub-agent delegation context.
    void set_todo_invoker(TodoInvoker cb) {
        todo_invoker_cb_ = std::move(cb);
    }

    // /lesson bridge. Without it, returns ERR.
    void set_lesson_invoker(LessonInvoker cb) {
        lesson_invoker_cb_ = std::move(cb);
    }

    // Injects remote A2A agent roster into the master's turn preamble.
    using RemoteRosterProvider = std::function<std::string()>;
    void set_remote_roster_provider(RemoteRosterProvider cb) {
        remote_roster_cb_ = std::move(cb);
    }

    // /mem read|write|clear bridge; falls back to filesystem when null.
    void set_memory_scratchpad(MemoryScratchpadInvoker cb) {
        memory_scratchpad_cb_ = std::move(cb);
    }

    // /search bridge. Without it, returns ERR.
    void set_search_invoker(SearchInvoker cb) { search_invoker_cb_ = std::move(cb); }

    // /write --persist, /read, /list bridges. Without them, persist degrades to SSE-only.
    void set_artifact_writer(ArtifactWriter cb) { artifact_writer_cb_ = std::move(cb); }
    void set_artifact_reader(ArtifactReader cb) { artifact_reader_cb_ = std::move(cb); }
    void set_artifact_lister(ArtifactLister cb) { artifact_lister_cb_ = std::move(cb); }

    // Disable /exec; agents receive a tool result explaining the ban.
    void set_exec_disabled(bool v) { exec_disabled_ = v; }

    // Bind /exec to a per-tenant sandbox (e.g. SandboxManager-backed
    // container).  When set, the dispatcher routes /exec through this
    // callback instead of the host's cmd_exec and the exec_disabled flag
    // is bypassed (a wired sandbox is sufficient to permit the writ).
    // Without it, /exec falls back to the host path gated by exec_disabled.
    void set_exec_invoker(ExecInvoker cb) { exec_invoker_cb_ = std::move(cb); }

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

    // Fired when an agent's gate-mode advisor returns HALT (or when the
    // redirect budget is exhausted and the runtime synthesises one).  Sibling
    // of stream_end_cb_, NOT a replacement — escalation_cb_ fires first with
    // the halt reason, then stream_end_cb_(ok=false) fires as for any other
    // failed turn.  Subscribers wire this to a UI affordance that surfaces
    // the reason out-of-band from the agent's normal text stream (REPL
    // banner, SSE `escalation` event, TUI overlay, etc.).
    using EscalationCallback = std::function<void(const std::string& agent_id,
                                                   int stream_id,
                                                   const std::string& halt_reason)>;
    void set_escalation_callback(EscalationCallback cb) { escalation_cb_ = std::move(cb); }

    // Advisor activity stream.  Fires for every advisor interaction so a
    // verbose log / SSE consumer can show gate decisions as they happen
    // — distinct from escalation_cb_ (which only fires on terminal HALT).
    //
    //   kind="consult"        executor invoked /advise; `detail` = question
    //   kind="gate_continue"  runtime gate accepted the terminating turn
    //   kind="gate_redirect"  gate rerouted the executor; `detail` = guidance
    //   kind="gate_halt"      gate halted the executor; `detail` = reason
    //                         (escalation_cb_ also fires for this case at depth 0)
    //   kind="gate_budget"    redirect budget exhausted; `detail` = budget reason
    //
    // For gate events, `preview` carries the first ~120 chars of the
    // executor's terminating turn so an operator can see what the gate
    // was reacting to without spelunking through the full transcript.
    struct AdvisorEvent {
        std::string agent_id;
        int         stream_id = 0;
        std::string kind;
        std::string detail;
        std::string preview;       // executor's terminating-turn text, truncated
        bool        malformed = false;
    };
    using AdvisorEventCallback = std::function<void(const AdvisorEvent&)>;
    void set_advisor_event_callback(AdvisorEventCallback cb) { advisor_event_cb_ = std::move(cb); }

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
    // `original_query` pins the advisor gate's [ORIGINAL TASK] context across
    // self-prompt continuations.  When empty, defaults to `message` (the
    // first turn of a fresh request).  LoopManager passes the loop's initial
    // prompt here on every iteration after the first.
    ApiResponse send(const std::string& agent_id, const std::string& message,
                     const std::string& original_query = "");

    // Streaming variant of send() — streams first turn via callback,
    // falls back to non-streaming for tool-call re-entry turns.
    ApiResponse send_streaming(const std::string& agent_id,
                               const std::string& message,
                               StreamCallback cb,
                               const std::string& original_query = "");
    // Multipart variant — used when the inbound user message includes
    // image parts (vision input).  Internally identical to the string
    // variant; the string overload wraps in a single text part.  The
    // master-text-gating, tool-call re-entry, and advisor-gate flows
    // all run unchanged on top of this.
    ApiResponse send_streaming(const std::string& agent_id,
                               std::vector<ContentPart> parts,
                               StreamCallback cb,
                               const std::string& original_query = "");

    // Ask index (master) about system state — used by the TCP server.
    ApiResponse ask_arbiter(const std::string& query);

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

    // Clear index master and all loaded agent histories.
    void reset_all_histories();

    // Token tracking — counts the shared client only.  Per-child ApiClients
    // created for /parallel turns track their own counters independently, so
    // these totals undercount tokens spent in parallel turns.  Per-turn cost
    // attribution via cost_cb_ is unaffected (it reads ApiResponse directly).
    int total_input_tokens()  const { return client_.total_input_tokens(); }
    int total_output_tokens() const { return client_.total_output_tokens(); }

    ApiClient& client() { return client_; }

    // Interrupt any in-progress API call across the master and all agents.
    // Thread-safe — can be called from the readline/main thread while the
    // exec thread is blocked in a streaming read.
    void cancel();

    // Run a user-typed slash command through the same tool dispatch path
    // agents use during a turn. Returns formatted tool-result text, or empty
    // if the line didn't parse to any command.
    std::string execute_slash_command(const std::string& line,
                                      const std::string& agent_id);

    // Build an AdvisorInvoker bound to a specific caller.  Returns a lambda
    // that makes a one-shot, history-less call against the caller's
    // configured advisor_model (from the caller's Constitution).  If the
    // caller has no advisor_model set, the returned lambda returns an
    // ERR string explaining the misconfiguration.
    //
    // Public so external callers (e.g. the api_server's structured-memory
    // reader using this for `/mem search --rerank`) can issue advisor
    // calls scoped to the calling agent without going through a slash
    // command.  Cost attribution still flows through cost_cb_.
    AdvisorInvoker make_advisor_invoker(const std::string& caller_id);

    // Build an AdvisorGateInvoker bound to a specific caller.  Distinct from
    // make_advisor_invoker: the gate variant takes structured input
    // (original task, terminating-turn text, tool summary) and parses a
    // CONTINUE/REDIRECT/HALT signal from the advisor's reply.  Returns a
    // closure that ERR-equivalent-halts when no advisor model is configured
    // — callers are expected to gate on `Constitution::AdvisorConfig::mode
    // == "gate"` before calling, so this is a defence-in-depth check.  Cost
    // is attributed to caller_id with the advisor model's pricing, the same
    // way /advise consultations are billed.
    AdvisorGateInvoker make_advisor_gate_invoker(const std::string& caller_id);

private:
    ApiClient client_;
    // Stored so make_parallel_invoker can create per-child ApiClient instances
    // (each with its own connection pool) instead of sharing the parent's
    // conn_mutex_.  Keys are plaintext — same exposure as the constructor arg.
    std::map<std::string, std::string> api_keys_;
    // Non-owning pointers to child ApiClients active during a /parallel turn.
    // cancel() iterates this under parallel_clients_mu_ so a cancel request
    // reaches in-flight parallel children, not just the parent client.
    std::mutex parallel_clients_mu_;
    std::vector<ApiClient*> parallel_clients_;

    std::unordered_map<std::string, std::unique_ptr<Agent>> agents_;
    mutable std::mutex agents_mutex_;
    std::string memory_dir_;
    ProgressCallback   progress_cb_;
    CostCallback       cost_cb_;
    AgentStartCallback start_cb_;
    ConfirmFn          confirm_cb_;
    ToolStatusFn       tool_status_cb_;
    PaneSpawner        pane_spawner_cb_;
    WriteInterceptor   write_interceptor_cb_;
    StructuredMemoryReader structured_memory_reader_cb_;
    StructuredMemoryWriter structured_memory_writer_cb_;
    MCPInvoker         mcp_invoker_cb_;
    A2AInvoker         a2a_invoker_cb_;
    SchedulerInvoker   scheduler_invoker_cb_;
    TodoInvoker        todo_invoker_cb_;
    LessonInvoker      lesson_invoker_cb_;
    RemoteRosterProvider remote_roster_cb_;
    MemoryScratchpadInvoker memory_scratchpad_cb_;
    SearchInvoker      search_invoker_cb_;
    ArtifactWriter     artifact_writer_cb_;
    ArtifactReader     artifact_reader_cb_;
    ArtifactLister     artifact_lister_cb_;
    ExecInvoker        exec_invoker_cb_;
    bool               exec_disabled_ = false;

    StreamStartCallback stream_start_cb_;
    AgentStreamCallback agent_stream_cb_;
    StreamEndCallback   stream_end_cb_;
    EscalationCallback  escalation_cb_;
    AdvisorEventCallback advisor_event_cb_;
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

} // namespace arbiter
