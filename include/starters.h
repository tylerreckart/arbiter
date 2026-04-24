#pragma once
// index/include/starters.h
//
// Registry of starter example agents shared between `index --init` and the
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
// to mutate the Constitutions and save them to ~/.index/agents/<id>.json.
std::vector<StarterAgent> starter_agents();

} // namespace index_ai
