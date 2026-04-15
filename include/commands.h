#pragma once
// index/include/commands.h — Agent-invocable command execution

#include <string>
#include <vector>
#include <functional>

namespace index_ai {

struct AgentCommand {
    std::string name;    // "fetch", "mem", "exec", "agent", "write"
    std::string args;    // rest of the command line
    std::string content; // multiline body (used by /write)
};

// Parse /command lines from an agent response.
// Skips lines inside ``` or ~~~ code fences.
std::vector<AgentCommand> parse_agent_commands(const std::string& response);

// Fetch a URL via curl. Returns content or "ERR: ..." on failure.
std::string cmd_fetch(const std::string& url);

// Execute a shell command. Returns stdout+stderr, or "ERR: ..." on failure.
// Output is capped at 32 KB. Exit status appended if non-zero.
std::string cmd_exec(const std::string& command);

// Write content to a file at path (creates parent directories).
// Returns "OK: wrote N bytes to <path>" or "ERR: ...".
std::string cmd_write(const std::string& path, const std::string& content);

// Read the agent's persistent memory file. Returns "" if none.
std::string cmd_mem_read(const std::string& agent_id, const std::string& memory_dir);

// Append a timestamped note to the agent's memory file.
void cmd_mem_write(const std::string& agent_id, const std::string& text,
                   const std::string& memory_dir);

// Delete the agent's memory file.
void cmd_mem_clear(const std::string& agent_id, const std::string& memory_dir);

// Shared scratchpad — pipeline-scoped memory visible to all agents.
// Stored at memory_dir/shared.md so any agent can read what another wrote.
std::string cmd_mem_shared_read(const std::string& memory_dir);
void        cmd_mem_shared_write(const std::string& text, const std::string& memory_dir);
void        cmd_mem_shared_clear(const std::string& memory_dir);

// Callback for agent-to-agent invocation: given (sub_agent_id, message),
// returns the sub-agent's response text or an "ERR: ..." string.
using AgentInvoker = std::function<std::string(const std::string&, const std::string&)>;

// Gatekeeper for potentially-destructive operations.  Given a human-readable
// prompt (e.g. "write agents/foo.md?"), returns true to proceed, false to
// abort.  If unset, every guarded command runs without prompting.
using ConfirmFn = std::function<bool(const std::string& prompt)>;

// True if `cmd` matches a pattern we always want to confirm before exec'ing
// (rm, rm -rf, redirects, sudo, mkfs, git force-push, find -delete, etc.).
// Conservative — misses creative destruction, but catches the common footguns.
bool is_destructive_exec(const std::string& cmd);

// Execute a parsed command list and return a [TOOL RESULTS] message
// suitable for feeding back to the agent.
// agent_invoker: optional — if provided, /agent commands are dispatched through it.
// confirm:       optional — gates /write (always) and destructive /exec.
std::string execute_agent_commands(const std::vector<AgentCommand>& cmds,
                                   const std::string& agent_id,
                                   const std::string& memory_dir,
                                   AgentInvoker agent_invoker = nullptr,
                                   ConfirmFn    confirm       = nullptr);

} // namespace index_ai
