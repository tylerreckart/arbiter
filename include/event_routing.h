#pragma once
// arbiter/include/event_routing.h
//
// Route POST /v1/events `type` values to an agent whose constitution
// lists a matching `event_types` glob.  File-backed agents live under
// agents_dir; tenant-stored agents are matched via the preloaded-list
// overload after the file scan misses.

#include <string>
#include <utility>
#include <vector>

namespace arbiter {

// Match event_type against an already-loaded list of (agent_id, event_types
// globs).  Returns the first matching agent_id, or "" when none match.
// Callers are responsible for iteration order (sort by agent_id for
// deterministic tenant routing).
std::string route_event(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& agents,
    const std::string& event_type);

// Scan *.json agent files in agents_dir for the first agent whose
// event_types array contains a glob matching event_type.  Returns that
// agent's id (filename stem, or Constitution::name when it is a distinct
// id), or "" if no match.
// Callers fall back to tenant routing and then "index".  Reads constitution
// files on each call — intended for the
// infrequent /v1/events path, not a hot loop.
std::string route_event(const std::string& agents_dir,
                        const std::string& event_type);

} // namespace arbiter
