#pragma once
// include/mcp/manager.h — Per-request MCP session manager
//
// The Manager owns a registry of named server configs and lazy-spawns
// a Client the first time the agent references each one.  Cached
// Clients are shared_ptrs: in-flight /mcp calls keep their session
// alive if a later acquire evicts a dead subprocess.  Remaining
// cache entries die when the Manager is destroyed (orchestrator
// teardown — i.e. when the /v1/orchestrate request ends).
//
// Why per-request and not long-lived?
//   • Tenant isolation — a stateful playwright session must not bleed
//     between tenants or between unrelated requests.
//   • Lifetime safety — a crashed server simply stops appearing in
//     subsequent requests; no zombie cleanup or health-check thread.
//   • Cost — cold starts hurt, but `npx @playwright/mcp` reuses its
//     installed browser binaries so the second run on a host is fast.

#include "mcp/client.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace arbiter::mcp {

// One server's static config — name, how to launch it.  Loaded from
// the registry file at startup.
struct ServerSpec {
    std::string              name;
    std::vector<std::string> argv;
    std::vector<std::string> env_extra;
    std::chrono::milliseconds init_timeout = std::chrono::seconds(60);
    std::chrono::milliseconds call_timeout = std::chrono::seconds(30);
};

// Read a registry JSON file at `path`.  Shape:
//   { "servers": { "playwright": { "command": "npx",
//                                    "args": ["@playwright/mcp@latest"],
//                                    "env": { "K": "V" } } } }
// Missing file ⇒ empty registry (the /mcp slash command then errors with
// a clear message).  Malformed JSON ⇒ throws so the operator notices at
// startup rather than mid-request.  Entries with an empty `command` are
// skipped (a single bad hand-edit must not brick the whole registry).
std::vector<ServerSpec> load_server_registry(const std::string& path);

// Pretty-print a registry for `~/.arbiter/mcp_servers.json`.  Round-trips
// with load_server_registry (command/args/env/timeouts).  env_extra entries
// must be "KEY=VALUE"; the first '=' splits key from value.
std::string serialize_server_registry(const std::vector<ServerSpec>& specs);

// Atomically write the registry to `path` (mode 0600).  Returns false on
// I/O failure.  Empty specs writes `{ "servers": {} }`.
bool save_server_registry(const std::string& path,
                          const std::vector<ServerSpec>& specs);

class Manager {
public:
    // Constructed from the registry; clients are *not* spawned here —
    // first reference triggers spawn.  Cheap to construct.
    explicit Manager(std::vector<ServerSpec> specs);
    ~Manager();

    Manager(const Manager&)            = delete;
    Manager& operator=(const Manager&) = delete;

    // True if a server with this name is configured (regardless of
    // whether it has been spawned yet).
    bool has(const std::string& name) const;

    // Configured server names, in registration order.  Used by /mcp tools
    // to enumerate the catalog before any subprocess has been spawned.
    std::vector<std::string> server_names() const;

    // Acquire (or spawn) the named client.  Throws if the name isn't
    // registered, or if spawn/handshake fails.
    //
    // Returns a shared_ptr so the caller keeps the Client alive even if a
    // later acquire evicts a dead subprocess from the cache.  /parallel
    // children share one Manager and can call /mcp concurrently; each
    // invoker holds its own ref for the duration of tools()/call_tool(),
    // so destroying the cache entry cannot UAF an in-flight call.
    //
    // A cached Client whose subprocess has exited is dropped from the
    // map and replaced with a fresh session on the next acquire.
    // Liveness is checked outside the Manager lock (it takes the
    // Client's rpc_mu_) so an in-flight cancel cannot deadlock the
    // cache or waitpid the child twice.  In-flight callers of the
    // dead Client still see their own shared_ptr (and get a protocol
    // error on the next RPC).
    std::shared_ptr<Client> client(const std::string& name);

private:
    std::shared_ptr<std::mutex> init_gate_for(const std::string& name);

    std::vector<ServerSpec>                                specs_;
    std::map<std::string, std::shared_ptr<Client>>         clients_;
    std::map<std::string, std::shared_ptr<std::mutex>>     init_gates_;
    mutable std::mutex                                     mu_;
};

} // namespace arbiter::mcp
