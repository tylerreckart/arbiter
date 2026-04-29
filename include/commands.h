#pragma once
// index/include/commands.h — Agent-invocable command execution

#include <string>
#include <vector>
#include <functional>
#include <map>

namespace index_ai {

struct AgentCommand {
    std::string name;    // "fetch", "mem", "exec", "agent", "write", "advise"
    std::string args;    // rest of the command line
    std::string content; // multiline body (used by /write)
    // True when a /write block was opened but /endwrite was never seen — the
    // model's response was cut off mid-file.  Caller should request a
    // continuation before executing the write to avoid persisting a partial.
    bool truncated = false;
};

// Parse /command lines from an agent response.
// Skips lines inside ``` or ~~~ code fences.
std::vector<AgentCommand> parse_agent_commands(const std::string& response);

// Fetch a URL via curl. Returns content or "ERR: ..." on failure.
std::string cmd_fetch(const std::string& url);

// Execute a shell command. Returns stdout+stderr, or "ERR: ..." on failure.
// Output is capped at 32 KB. Exit status appended if non-zero.
// Destructive commands are blocked unless confirmed=true (gate already passed).
std::string cmd_exec(const std::string& command, bool confirmed = false);

// Write content to a file at path (creates parent directories).
// Returns "OK: wrote N bytes to <path>" or "ERR: ...".
std::string cmd_write(const std::string& path, const std::string& content);

// Read the agent's persistent memory file. Returns "" if none.
std::string cmd_mem_read(const std::string& agent_id, const std::string& memory_dir);

// Append a timestamped note to the agent's memory file.
// Returns "OK: ..." on success or "ERR: ..." on failure.
std::string cmd_mem_write(const std::string& agent_id, const std::string& text,
                          const std::string& memory_dir);

// Delete the agent's memory file.
void cmd_mem_clear(const std::string& agent_id, const std::string& memory_dir);

// Shared scratchpad — pipeline-scoped memory visible to all agents.
// Stored at memory_dir/shared.md so any agent can read what another wrote.
std::string cmd_mem_shared_read(const std::string& memory_dir);
std::string cmd_mem_shared_write(const std::string& text, const std::string& memory_dir);
std::string cmd_mem_shared_clear(const std::string& memory_dir);

// Callback for agent-to-agent invocation: given (sub_agent_id, message),
// returns the sub-agent's response text or an "ERR: ..." string.
using AgentInvoker = std::function<std::string(const std::string&, const std::string&)>;

// Callback for fan-out: given N (sub_agent_id, message) pairs, runs them
// concurrently on separate threads, each with its own dedup cache (a
// shared cache would be a std::map data race), and returns their results
// in input order.  The orchestrator owns the threading + history-conflict
// checks; commands.cpp just hands it the child list.  Empty vector ⇒ the
// orchestrator rejected the batch (e.g., duplicate agent_ids).
using ParallelInvoker = std::function<std::vector<std::string>(
    const std::vector<std::pair<std::string, std::string>>& children)>;

// Callback for advisor consultation: given a question string, fires a
// one-shot, history-less API call against the calling agent's configured
// advisor_model and returns the advisor's reply (or an "ERR: ..." string).
// The question is opaque to the invoker — advisor sees ONLY what the
// executor wrote, no prior turn context leaks in.  Replaces the Anthropic
// `advisor_20260301` beta tool with a provider-agnostic text convention,
// so ollama/* executors can pair with claude-* advisors (or vice versa)
// through the same ApiClient's prefix-based routing.
using AdvisorInvoker = std::function<std::string(const std::string& question)>;

// Gatekeeper for potentially-destructive operations.  Given a human-readable
// prompt (e.g. "write agents/foo.md?"), returns true to proceed, false to
// abort.  If unset, every guarded command runs without prompting.
using ConfirmFn = std::function<bool(const std::string& prompt)>;

// Fired once per executed /cmd, after execution completes, with the command
// name ("fetch", "exec", "write", "agent", "mem", "advise") and whether the
// result indicates success.  Success = the command did what it advertised;
// failure = ERR: prefix, UPSTREAM FAILED, user-declined, budget-skipped, or
// exec non-zero exit status.  The REPL wires this to ToolCallIndicator so
// the spinner's ✓/✗ summary reflects real post-exec status, not just a
// count of /cmd lines in the stream.
using ToolStatusFn = std::function<void(const std::string& kind, bool ok)>;

// Spawn a new UI pane running `agent_id` with `message` as its first queued
// input.  Fire-and-forget from the caller's perspective: the spawning agent
// gets a short "OK: ..." / "ERR: ..." back and continues its own turn while
// the new pane runs in parallel on its own exec thread.  Unlike /agent
// (which blocks for the sub-agent's reply and folds it into the caller's
// context), /pane is best for truly independent work — the results stream
// into the new pane's own view and do NOT return to the spawner.
using PaneSpawner = std::function<std::string(const std::string& agent_id,
                                                const std::string& message)>;

// Intercept /write — when set, the write path routes the file content
// through this callback instead of touching the server filesystem.  The
// callback returns the tool-result text that the calling agent will see
// (typically "OK: captured N bytes for 'path' …" or an "ERR: …").  Used
// by the HTTP API to emit the file contents as an SSE event so the
// client receives the artifact without the server needing to persist it.
using WriteInterceptor = std::function<std::string(const std::string& path,
                                                    const std::string& content)>;

// Read-only window into the tenant's structured-memory store from inside a
// turn.  When set, the slash-command dispatcher exposes:
//   /mem entries [type[,type...]]   — list entries (optional comma-sep type filter)
//   /mem entry <id>                 — one entry's full content + its edges
//   /mem search <query>             — substring match on title + content
// The callback receives the subcommand kind ("entries" | "entry" | "search")
// and the rest of the line, and returns the pre-formatted body that goes
// into the [/mem ...] tool-result block (without the [/mem ...] header).
// Reads only — see StructuredMemoryWriter for the write half.  Without this
// callback wired, the dispatcher returns ERR so the agent adapts.
using StructuredMemoryReader = std::function<std::string(const std::string& kind,
                                                          const std::string& args)>;

// Write window for agent-contributed entries and links.  When set, the
// dispatcher exposes:
//   /mem propose entry <type> <title>           — propose a new typed node
//   /mem propose link <src_id> <relation> <dst_id> — propose a directed edge
// All writes land in the proposal queue with status='proposed' — they do
// NOT show up in the curated graph reads (or in the agent's own reads via
// /mem entries|entry|search) until a human accepts them through the HTTP
// surface.  The callback receives the subcommand kind ("propose-entry" |
// "propose-link") and the rest of the line, and returns the formatted body
// for the [/mem propose ...] tool-result block.  Without this callback
// wired, the dispatcher returns ERR.
using StructuredMemoryWriter = std::function<std::string(const std::string& kind,
                                                          const std::string& args)>;

// Replaces the filesystem file-scratchpad path with a tenant-scoped
// DB-backed implementation when set.  Without this callback the /mem
// read|write|clear and /mem shared read|write|clear dispatch falls
// back to cmd_mem_* on the local filesystem (the CLI/REPL path).
//
// Operations:
//   "read"          — read this agent's scratchpad → markdown content
//   "write"         — append `args` to this agent's scratchpad → "OK: ..."
//   "clear"         — clear this agent's scratchpad → ""
//   "shared-read"   — read the shared scratchpad → markdown
//   "shared-write"  — append `args` to the shared scratchpad → "OK: ..."
//   "shared-clear"  — clear the shared scratchpad → "OK"
// The callback receives the calling agent's id (used by per-agent ops;
// ignored by shared-*) and the inline args (text for writes, "" for
// reads/clears).  Tenant scoping happens inside the callback via closure.
using MemoryScratchpadInvoker = std::function<std::string(
    const std::string& op,
    const std::string& agent_id,
    const std::string& args)>;

// Persistent artifact write — bridges /write --persist to the
// per-conversation artifact store.  Returns the formatted body for
// the [/write --persist <path>] tool-result block (typically
// "OK: persisted N bytes (artifact #ID, K of LIMIT bytes used in
// conversation)").  When set, the dispatcher fires this AFTER the
// existing WriteInterceptor (so the SSE `file` event still goes out
// for the live UI) and AFTER any /write confirmation gate.
//
// Errors come back as "ERR: ..." which the dispatcher caches as a
// failed call so the agent doesn't infinite-loop on a quota cap.
using ArtifactWriter = std::function<std::string(const std::string& path,
                                                  const std::string& content)>;

// Persistent artifact read.  When set, /read <path> resolves the
// agent's request against the conversation's artifact namespace and
// returns the body for the [/read <path>] block — file content on hit,
// "ERR: ..." on miss / path-rejection.  Without this the dispatcher
// returns ERR; CLI/REPL contexts leave it null.
using ArtifactReader = std::function<std::string(const std::string& path)>;

// Persistent artifact listing.  When set, /list returns one path per
// line with size + updated_at metadata so the agent can plan
// follow-up work without re-reading.  Returns an empty string when
// the conversation has no persisted artifacts (the dispatcher then
// surfaces "(no artifacts)").
using ArtifactLister = std::function<std::string()>;

// Web-search bridge.  When set, /search <query> [top=N] routes through
// this callback; the API server wires it to the configured provider
// (Brave in v1).  CLI/REPL contexts leave it null → /search returns
// ERR with a clear "configure ApiServerOptions::search_api_key" message
// so agents adapt without trying again.
//
// The callback receives the raw query string and a result count
// (defaulted to 10 when the agent didn't specify), and returns the
// pre-formatted body for the [/search ...] tool-result block — one
// numbered result per line: "<n>. <title> — <snippet>\n   <url>\n".
// Errors come back as "ERR: ..." which the dispatcher caches as failed.
using SearchInvoker = std::function<std::string(const std::string& query,
                                                  int top_n)>;

// Bridge to the per-request MCP session manager.  Drives the agent-
// facing /mcp slash surface:
//   /mcp tools                       — list every configured server's tools
//   /mcp tools <server>              — list one server's tools
//   /mcp call  <server> <tool> [json] — invoke a tool with optional JSON args
// The callback receives the subcommand kind ("tools" | "call") and the
// rest of the line, and returns the body for the [/mcp ...] tool-result
// block.  Spawning + lifecycle is the callback's concern (the
// orchestrator owns an MCP Manager whose clients die when the request
// ends).  Without this callback, the dispatcher returns ERR — same
// pattern as the structured-memory readers/writers.
using MCPInvoker = std::function<std::string(const std::string& kind,
                                              const std::string& args)>;

// True if `cmd` matches a pattern we always want to confirm before exec'ing
// (rm, rm -rf, redirects, sudo, mkfs, git force-push, find -delete, etc.).
// Conservative — misses creative destruction, but catches the common footguns.
bool is_destructive_exec(const std::string& cmd);

// Execute a parsed command list and return a [TOOL RESULTS] message
// suitable for feeding back to the agent.
// agent_invoker: optional — if provided, /agent commands are dispatched through it.
// confirm:       optional — gates /write (always) and destructive /exec.
// dedup_cache:   optional — keyed by (cmd|args[|content-hash]); when a command
//                repeats within the same cache, the second call is NOT dispatched;
//                instead a synthetic DUPLICATE block is emitted quoting the prior
//                result.  Caller owns the map and should clear/reset it between
//                independent top-level user requests.
// advisor_invoker: optional — if provided, /advise commands are dispatched
//                  through it.  Without one, /advise returns an ERR.
std::string execute_agent_commands(const std::vector<AgentCommand>& cmds,
                                   const std::string& agent_id,
                                   const std::string& memory_dir,
                                   AgentInvoker agent_invoker = nullptr,
                                   ConfirmFn    confirm       = nullptr,
                                   std::map<std::string, std::string>* dedup_cache = nullptr,
                                   AdvisorInvoker advisor_invoker = nullptr,
                                   ToolStatusFn   tool_status     = nullptr,
                                   PaneSpawner    pane_spawner    = nullptr,
                                   WriteInterceptor write_interceptor = nullptr,
                                   bool           exec_disabled   = false,
                                   ParallelInvoker parallel_invoker = nullptr,
                                   StructuredMemoryReader structured_memory_reader = nullptr,
                                   StructuredMemoryWriter structured_memory_writer = nullptr,
                                   MCPInvoker     mcp_invoker     = nullptr,
                                   MemoryScratchpadInvoker memory_scratchpad = nullptr,
                                   SearchInvoker  search_invoker  = nullptr,
                                   ArtifactWriter artifact_writer = nullptr,
                                   ArtifactReader artifact_reader = nullptr,
                                   ArtifactLister artifact_lister = nullptr);

// True if a tool-result block indicates the command failed.  Pattern-matches
// the ERR:/UPSTREAM FAILED/SKIPPED framing used throughout execute_agent_commands.
// Exposed for testing; callers normally just wire a ToolStatusFn and let
// execute_agent_commands invoke it.
bool is_tool_result_failure(const std::string& block);

} // namespace index_ai
