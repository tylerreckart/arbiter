#pragma once
// include/mcp/client.h — Single-server MCP session
//
// One Client wraps one Subprocess running an MCP server (typically
// `npx @playwright/mcp`).  The constructor performs the JSON-RPC
// initialize handshake; subsequent calls pump tools/list and
// tools/call requests synchronously.
//
// Stateful by design: a stateful playwright session means navigate +
// click + snapshot share the same browser context.  Per-request
// orchestrators construct one Client per server name as the agent
// references it; destructor kills the subprocess.

#include "mcp/subprocess.h"
#include "mcp/types.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace index_ai::mcp {

struct ClientConfig {
    std::string              name;        // logical name ("playwright")
    std::vector<std::string> argv;        // ["npx", "@playwright/mcp"]
    std::vector<std::string> env_extra;   // optional KEY=VAL strings
    // Per-call wall clock for tools/call.  Playwright snapshots and
    // navigation can take 10s+; init handshake gets a longer budget
    // since `npx` may need to download packages on first run.
    std::chrono::milliseconds init_timeout = std::chrono::seconds(60);
    std::chrono::milliseconds call_timeout = std::chrono::seconds(30);
};

class Client {
public:
    // Spawns the subprocess and runs the initialize handshake.  Throws
    // std::runtime_error on subprocess spawn failure, handshake timeout,
    // or protocol-level error response.  The subprocess is killed
    // automatically on throw — caller doesn't need a try/catch around
    // `tools/call` to clean up after a failed init.
    explicit Client(ClientConfig cfg);
    ~Client();

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    const std::string& name() const { return cfg_.name; }

    // Cached after first call; refresh by recreating the Client.  The
    // MCP spec supports tools/list_changed notifications for hot-reload
    // but arbiter's per-request lifetime makes that moot.
    const std::vector<ToolDescriptor>& tools();

    // Synchronous tool call.  Returns the parsed result; throws on
    // protocol-level failures (bad framing, server crash, timeout).
    // Tool-level errors (isError=true in the response) come back inside
    // ToolResult so the agent can read them like any other tool output.
    ToolResult call_tool(const std::string& name,
                          std::shared_ptr<JsonValue> arguments);

private:
    ClientConfig                   cfg_;
    std::unique_ptr<Subprocess>    proc_;
    int64_t                        next_id_ = 1;
    std::vector<ToolDescriptor>    tools_cache_;
    bool                           tools_loaded_ = false;

    // Send a request, read its matching response (skipping inbound
    // server notifications and stale responses).  Throws on timeout
    // or protocol error.
    Response rpc(const std::string& method,
                  std::shared_ptr<JsonValue> params,
                  std::chrono::milliseconds timeout);
};

} // namespace index_ai::mcp
