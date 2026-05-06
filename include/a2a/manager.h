#pragma once
// include/a2a/manager.h — Per-request manager for remote A2A agents.
//
// Mirrors mcp::Manager — owns a registry of named remote agents and
// constructs one Client per agent on first reference.  The lifecycle
// is bound to the per-request orchestrator: the Manager dies when the
// orchestrator does, no long-lived background state.  HTTP keepalive
// across requests is intentionally NOT plumbed for v1 — request-scoped
// clients keep tenant isolation simple, and arbiter doesn't issue
// enough back-to-back A2A calls per request for the connection-reuse
// win to matter yet.

#include "a2a/client.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace index_ai::a2a {

// One remote agent's static config.  `endpoint_url` is the JSON-RPC
// POST URL (the .well-known card lives at <endpoint_url>/agent-card.json).
// `bearer_token` is the resolved token — manager construction reads it
// from `token_env` at registry-load time.
struct RemoteAgentConfig {
    std::string name;
    std::string endpoint_url;
    std::string bearer_token;
};

// Read a registry JSON file at `path`.  Shape:
//   { "agents": { "weatherbot": { "url": "https://...",
//                                  "auth": { "type": "bearer",
//                                            "token_env": "WEATHERBOT_TOKEN" } } } }
// Missing file ⇒ empty registry.  Malformed file ⇒ throws so the
// operator notices at startup rather than mid-request.  Tokens that
// resolve to empty strings (env var unset) are still kept — the client
// will surface auth errors when it tries to call.
std::vector<RemoteAgentConfig> load_registry(const std::string& path);

class Manager {
public:
    explicit Manager(std::vector<RemoteAgentConfig> configs);
    ~Manager() = default;

    Manager(const Manager&)            = delete;
    Manager& operator=(const Manager&) = delete;

    // True if a remote agent with this name is configured.
    bool has(const std::string& name) const;

    // Configured names, in registration order.
    std::vector<std::string> agent_names() const;

    // Acquire (or construct) the named client.  Throws if the name
    // isn't registered.  The returned reference is valid for the
    // lifetime of the Manager.
    Client& client(const std::string& name);

    // Best-effort fetch of all configured agents' cards.  Skips agents
    // whose card fetch fails — the per-agent error is logged to stderr
    // but doesn't stop the rest from being collected.  Used by the
    // index-routing layer (PR-8) to inject remote-agent capabilities
    // into the master agent's system prompt.
    std::vector<AgentCard> cards();

private:
    std::vector<RemoteAgentConfig>                  configs_;
    std::map<std::string, std::unique_ptr<Client>>  clients_;
    mutable std::mutex                              mu_;
};

} // namespace index_ai::a2a
