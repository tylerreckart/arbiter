// src/a2a/server.cpp — Pure transforms for the server-side A2A surface.
//
// No I/O lives here.  The HTTP handlers in api_server.cpp call these
// functions to build agent cards and resolve public URLs; keeping this
// module side-effect-free makes the card builder cheap to test in
// isolation.

#include "a2a/server.h"

#include "json.h"

#include <map>
#include <string>

namespace index_ai::a2a {

namespace {

// Friendly metadata for arbiter slash-command capabilities.  Used to
// project Constitution.capabilities into A2A Skill entries with names
// and descriptions a remote client UI can render without knowing
// arbiter-internal jargon.  Unknown capabilities fall back to a synthetic
// description ("custom capability '<x>'") so future additions don't
// require a code change to surface — they just look terser.
struct SkillSpec {
    const char* name;
    const char* description;
};

const std::map<std::string, SkillSpec>& skill_catalog() {
    static const std::map<std::string, SkillSpec> kSpecs = {
        {"/fetch",    {"fetch-url",     "fetch a URL and return its rendered text content"}},
        {"/search",   {"web-search",    "run a web search query and summarize the top results"}},
        {"/browse",   {"browse-page",   "open a URL in a browser and extract structured page content"}},
        {"/exec",     {"shell-exec",    "execute a shell command in a sandboxed environment (gated)"}},
        {"/write",    {"write-file",    "create or replace a file in the conversation's artifact store"}},
        {"/read",     {"read-file",     "read a file from the conversation's artifact store"}},
        {"/list",     {"list-files",    "list files in the conversation's artifact store"}},
        {"/mem",      {"memory",        "read, write, and search the agent's persistent memory"}},
        {"/mcp",      {"mcp-tools",     "invoke tools exposed by configured MCP servers"}},
        {"/agent",    {"delegate",      "delegate a sub-task to another arbiter agent"}},
        {"/parallel", {"parallel-fanout","dispatch the same prompt to multiple agents in parallel"}},
        {"/advise",   {"advisor-consult","consult a higher-capability advisor model mid-turn"}},
    };
    return kSpecs;
}

Skill make_skill_from_capability(const std::string& cap) {
    Skill s;
    auto& cat = skill_catalog();
    auto it = cat.find(cap);
    if (it != cat.end()) {
        // Stable ids: the friendly catalog name.  Lets remote
        // orchestrators reference skills by short handle.
        s.id          = it->second.name;
        s.name        = it->second.name;
        s.description = it->second.description;
    } else {
        std::string clean = cap;
        if (!clean.empty() && clean[0] == '/') clean = clean.substr(1);
        s.id          = clean.empty() ? "capability" : clean;
        s.name        = s.id;
        s.description = "custom arbiter capability '" + cap + "'";
    }
    s.tags = { cap };          // raw slash-command form so callers can filter
    return s;
}

// Every agent gets a synthetic "chat" skill regardless of declared
// capabilities — A2A clients route by skill id, and a free-form text
// channel is the universal fallback.  Ordered first so naive skill
// pickers default to it.
Skill chat_skill() {
    Skill s;
    s.id          = "chat";
    s.name        = "chat";
    s.description = "free-form text conversation with the agent";
    s.tags        = { "text" };
    return s;
}

// Standard bearer-token security blocks shared by the real per-agent
// cards and the well-known stub.  Built fresh each call because the
// shared_ptr<JsonValue> is owned by whoever consumes it; sharing across
// cards risks accidental mutation.
void attach_bearer_security(AgentCard& c) {
    auto schemes = jobj();
    auto bearer  = jobj();
    auto& bm = bearer->as_object_mut();
    bm["type"]   = jstr("http");
    bm["scheme"] = jstr("bearer");
    schemes->as_object_mut()["bearer"] = bearer;
    c.security_schemes = schemes;

    auto sec = jarr();
    auto requirement = jobj();
    requirement->as_object_mut()["bearer"] = jarr();
    sec->as_array_mut().push_back(requirement);
    c.security = sec;
}

} // namespace

std::string resolve_public_base_url(const ApiServerOptions& opts,
                                    const std::map<std::string, std::string>& headers) {
    if (!opts.public_base_url.empty()) {
        std::string s = opts.public_base_url;
        // Defensive trailing-slash strip so callers can append "/v1/..."
        // without doubling the separator.
        while (!s.empty() && s.back() == '/') s.pop_back();
        return s;
    }
    auto it = headers.find("host");
    if (it == headers.end() || it->second.empty()) {
        // No Host header is either an HTTP/1.0 client or a buggy proxy;
        // fall back to a literal localhost binding so the card is at
        // least syntactically valid.  Operators with real deploys
        // should set public_base_url.
        return "http://localhost";
    }
    return "http://" + it->second;
}

AgentCard build_agent_card(const Constitution& cons,
                           const std::string& agent_id,
                           const std::string& server_base_url,
                           const std::string& version) {
    AgentCard c;
    c.protocol_version = "1.0";
    c.name             = cons.name.empty() ? agent_id : cons.name;

    // Description blends role + goal because A2A clients render the
    // single field; arbiter's role is a short label and the goal is the
    // sentence-of-purpose.  Skip empties without leaving stray separators.
    std::string desc = cons.role;
    if (!cons.goal.empty()) {
        if (!desc.empty()) desc += " — ";
        desc += cons.goal;
    }
    if (desc.empty()) desc = "arbiter agent";
    c.description = std::move(desc);

    c.url     = server_base_url + "/v1/a2a/agents/" + agent_id;
    c.version = version.empty() ? "1" : version;

    c.default_input_modes  = { "text/plain" };
    c.default_output_modes = { "text/plain", "application/json" };

    c.capabilities.streaming                = true;
    c.capabilities.push_notifications       = false;
    c.capabilities.state_transition_history = false;

    // One skill per declared capability, plus a universal "chat" skill
    // first.  Agents that declare no capabilities still expose chat.
    c.skills.push_back(chat_skill());
    for (auto& cap : cons.capabilities) {
        c.skills.push_back(make_skill_from_capability(cap));
    }

    attach_bearer_security(c);
    c.preferred_transport = "JSONRPC";
    return c;
}

AgentCard build_well_known_stub(const std::string& base_url) {
    AgentCard c;
    c.protocol_version = "1.0";
    c.name             = "arbiter";
    c.description      = "Arbiter multi-tenant A2A endpoint. Agents are tenant-scoped; "
                         "fetch /v1/a2a/agents/<agent_id>/agent-card.json with a tenant "
                         "bearer token to discover and call individual agents.";
    c.url              = base_url + "/v1/a2a";
    c.version          = "stub";
    c.default_input_modes  = { "text/plain" };
    c.default_output_modes = { "text/plain", "application/json" };
    c.capabilities.streaming           = true;
    c.capabilities.push_notifications  = false;

    Skill discover;
    discover.id          = "discover";
    discover.name        = "discover";
    discover.description = "exchange a tenant bearer token for per-agent cards at "
                           "/v1/a2a/agents/<agent_id>/agent-card.json";
    discover.tags        = { "discovery", "auth-required" };
    c.skills = { std::move(discover) };

    attach_bearer_security(c);
    c.preferred_transport = "JSONRPC";
    return c;
}

} // namespace index_ai::a2a
