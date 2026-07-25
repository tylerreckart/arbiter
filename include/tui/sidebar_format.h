#pragma once

#include <string>
#include <string_view>

namespace arbiter {

// Shared token/cost formatters for the TUI sidebar.

std::string format_token_count(int n);

// Returns USD estimate for the given model id.  Unknown / local models use
// zero cost; unrecognized hosted models fall back to Sonnet-equivalent rates.
double estimate_cost_usd(std::string_view model, int in_tokens, int out_tokens);

std::string format_cost_usd(double usd);

// Short label for the pricing table used (model id, "mixed", or "local").
std::string cost_basis_label(std::string_view primary_model, bool mixed_models);

// context_window_for_model / context_pct_value live in model_context.h
// (shared with Agent compaction).  Re-exported via that header for TUI code.

// Percentage string ("42%") or empty when context window is unknown.
std::string format_context_pct(int prompt_tokens, std::string_view model);

// Short display label for the session sidebar model row: keeps only the
// portion after the first `/` or `\` (e.g. `openrouter/openai/gpt-5.2` →
// `openai/gpt-5.2`). Unprefixed ids are returned unchanged.
std::string format_sidebar_model(std::string_view model);

} // namespace arbiter
