#pragma once
// Shared model catalogue: ids the runtime routes, providers, and context
// windows used by GET /v1/models, /model, the setup wizard, and compaction
// (via context_window_for_model).

#include <cstddef>
#include <string>
#include <string_view>

namespace arbiter {

struct ModelCatalogEntry {
    const char* id;
    const char* provider;     // e.g. "openrouter"
    int context_window;       // tokens; 0 = unknown / use char-budget fallback
    const char* blurb;        // one-liner for wizard / /model; may be ""
    bool recommended;         // first-run wizard pick list
};

// Full static catalogue (OpenRouter-hosted + short Claude aliases).
// Lifetime: process — pointers remain valid for the program duration.
const ModelCatalogEntry* model_catalog(std::size_t& count);

// Exact id match (case-sensitive).  nullptr when not listed.
const ModelCatalogEntry* find_model_catalog_entry(std::string_view id);

// Human-readable window ("1M", "200k", "128k", "unknown").
std::string format_context_window(int tokens);

// Multi-line catalogue listing for /model with no args.
std::string format_model_catalog_list();

} // namespace arbiter
