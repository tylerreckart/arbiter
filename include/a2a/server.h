#pragma once
// include/a2a/server.h — Pure transforms for the server-side A2A surface.
//
// This module contains the Constitution → AgentCard mapping and the URL
// resolver.  HTTP handlers live in api_server.cpp where they can use the
// translation-unit-local response writers, the orchestrator factory, and
// the tenant store without re-exposing TU internals.  Keeping this file
// I/O-free makes the card builder trivially testable.

#include "a2a/types.h"
#include "api_server.h"
#include "constitution.h"

#include <map>
#include <string>

namespace index_ai::a2a {

// Resolve the public base URL the agent card should advertise.  Looks at
// opts.public_base_url first; falls back to "http://" + Host header.  The
// trailing slash is *not* included in the result so callers can append
// "/v1/a2a/agents/<id>" cleanly.
std::string resolve_public_base_url(const ApiServerOptions& opts,
                                    const std::map<std::string, std::string>& headers);

// Map an arbiter Constitution onto an A2A AgentCard for serving.
// `agent_id` is the URL-facing handle (the index master is "index";
// stored agents use their tenant catalog id).  `version` is opaque —
// callers thread the agent's `updated_at` timestamp in so cached cards
// can be invalidated downstream.
AgentCard build_agent_card(const Constitution& c,
                           const std::string& agent_id,
                           const std::string& server_base_url,
                           const std::string& version);

// Build the unauth top-level discovery stub.  Returns a card whose URL
// points back at /v1/a2a (a list endpoint we don't yet serve; the
// description tells clients to fetch per-agent cards instead) and whose
// securitySchemes declare the bearer auth callers must use for the real
// endpoints.  v1.0 requires skills.length >= 1, so a synthetic "discover"
// skill is included.
AgentCard build_well_known_stub(const std::string& server_base_url);

} // namespace index_ai::a2a
