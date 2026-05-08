#pragma once
// include/mcp/manager.h — Per-request MCP session manager
//
// The Manager owns a registry of named server configs and lazy-spawns
// a Client the first time the agent references each one.  All clients
// die together when the Manager is destroyed (which happens at
// orchestrator teardown — i.e. when the /v1/orchestrate request ends).
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
// a clear message).  Malformed file ⇒ throws so the operator notices at
// startup rather than mid-request.
std::vector<ServerSpec> load_server_registry(const std::string& path);

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
    // registered, or if spawn/handshake fails.  The returned reference
    // is valid for the lifetime of the Manager.  Not thread-safe across
    // managers, but the per-request orchestrator funnels every /mcp
    // call through one thread so we don't need a finer-grained lock.
    Client& client(const std::string& name);

private:
    std::vector<ServerSpec>                            specs_;
    std::map<std::string, std::unique_ptr<Client>>     clients_;
    mutable std::mutex                                 mu_;
};

} // namespace arbiter::mcp
