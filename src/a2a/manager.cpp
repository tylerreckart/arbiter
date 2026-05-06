// src/a2a/manager.cpp — Per-request remote-agent manager + registry loader.

#include "a2a/manager.h"

#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace index_ai::a2a {

namespace {

// Read entire file into a string.  Returns empty string when the file
// doesn't exist — that's how we signal "registry unconfigured" without
// surfacing it as an error every startup for users who don't run any
// remote agents.
std::optional<std::string> slurp_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Resolve a token from the auth config block.  Two shapes accepted:
//   { "type": "bearer", "token": "literal" }       — token inline (test only)
//   { "type": "bearer", "token_env": "ENV_NAME" }  — recommended
// Other auth types fail at startup so the operator notices.
std::string resolve_bearer(const JsonValue& auth, const std::string& agent_name) {
    const std::string type = auth.get_string("type", "bearer");
    if (type != "bearer") {
        throw std::runtime_error("a2a registry: agent '" + agent_name +
                                  "' uses unsupported auth type '" + type +
                                  "' — only 'bearer' is implemented in v1");
    }
    if (auto t = auth.get("token"); t && t->is_string()) {
        return t->as_string();
    }
    if (auto e = auth.get("token_env"); e && e->is_string()) {
        const std::string env_name = e->as_string();
        const char* v = std::getenv(env_name.c_str());
        if (!v) {
            // Don't fail — we keep the empty token so the client
            // surfaces a clear auth error on first call instead of
            // crashing the whole server's startup.
            std::fprintf(stderr,
                "[a2a] warning: token_env '%s' for remote agent '%s' is unset; "
                "calls will fail with HTTP 401 until it's exported.\n",
                env_name.c_str(), agent_name.c_str());
            return "";
        }
        return v;
    }
    return "";   // no auth — caller hits unauth endpoints (rare)
}

} // namespace

std::vector<RemoteAgentConfig> load_registry(const std::string& path) {
    std::vector<RemoteAgentConfig> out;
    if (path.empty()) return out;

    auto text = slurp_file(path);
    if (!text) return out;        // file absent ⇒ empty registry

    std::shared_ptr<JsonValue> root;
    try { root = json_parse(*text); }
    catch (const std::exception& e) {
        throw std::runtime_error("a2a registry parse: " + std::string(e.what()));
    }
    if (!root || !root->is_object()) {
        throw std::runtime_error("a2a registry: top-level must be an object");
    }
    auto agents = root->get("agents");
    if (!agents) return out;       // no `agents` key ⇒ empty
    if (!agents->is_object()) {
        throw std::runtime_error("a2a registry: 'agents' must be an object");
    }

    for (auto& [name, spec] : agents->as_object()) {
        if (!spec || !spec->is_object()) {
            throw std::runtime_error("a2a registry: agent '" + name +
                                      "' is not an object");
        }
        const std::string url = spec->get_string("url", "");
        if (url.empty()) {
            throw std::runtime_error("a2a registry: agent '" + name +
                                      "' is missing required 'url'");
        }
        std::string bearer;
        if (auto auth = spec->get("auth"); auth && auth->is_object()) {
            bearer = resolve_bearer(*auth, name);
        }
        out.push_back({name, url, std::move(bearer)});
    }
    return out;
}

Manager::Manager(std::vector<RemoteAgentConfig> configs)
    : configs_(std::move(configs)) {}

bool Manager::has(const std::string& name) const {
    for (auto& c : configs_) {
        if (c.name == name) return true;
    }
    return false;
}

std::vector<std::string> Manager::agent_names() const {
    std::vector<std::string> out;
    out.reserve(configs_.size());
    for (auto& c : configs_) out.push_back(c.name);
    return out;
}

Client& Manager::client(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = clients_.find(name);
    if (it != clients_.end()) return *it->second;

    const RemoteAgentConfig* cfg = nullptr;
    for (auto& c : configs_) {
        if (c.name == name) { cfg = &c; break; }
    }
    if (!cfg) {
        throw std::runtime_error("a2a: no remote agent named '" + name + "'");
    }
    auto cli = std::make_unique<Client>(cfg->name, cfg->endpoint_url,
                                          cfg->bearer_token);
    Client& ref = *cli;
    clients_.emplace(name, std::move(cli));
    return ref;
}

std::vector<AgentCard> Manager::cards() {
    std::vector<AgentCard> out;
    for (auto& cfg : configs_) {
        try {
            Client& cli = client(cfg.name);
            out.push_back(cli.card());
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[a2a] warning: card fetch for remote agent '%s' failed: %s\n",
                cfg.name.c_str(), e.what());
        }
    }
    return out;
}

} // namespace index_ai::a2a
