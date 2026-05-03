#pragma once
// index/include/starters.h
//
// Registry of starter example agents shared between `arbiter --init` and the
// first-run wizard.  Each entry carries a default Constitution plus a short
// blurb suitable for menu display.
//
// The Constitutions are returned by value so callers (notably the wizard)
// can mutate the model / advisor_model / other fields before saving the
// customized agent to disk.

#include "constitution.h"

#include <string>
#include <vector>

namespace index_ai {

struct StarterAgent {
    std::string  id;       // "reviewer", "research", …
    std::string  blurb;    // one-line description shown in menus
    Constitution config;   // defaults (id, model, advisor_model, rules, …)
};

// Returns a fresh copy of every starter's default config.  Caller is free
// to mutate the Constitutions and save them to ~/.arbiter/agents/<id>.json.
std::vector<StarterAgent> starter_agents();

// Verbatim source-tree JSON for a given starter id, or empty when no such
// starter is bundled.  cmd_init uses this to write the original
// pretty-printed file on disk, which preserves field order, raw casing of
// the "advisor" field, and avoids the IEEE-754 round-trip noise that
// to_json() would introduce on `temperature` and other doubles.
std::string starter_json(const std::string& id);

} // namespace index_ai
