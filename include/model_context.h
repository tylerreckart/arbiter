#pragma once
// Shared model context-window helpers (TUI sidebar + Agent compaction).
// Listed catalogue ids (see model_catalog.h) are authoritative; everything
// else falls back to substring heuristics.

#include <string_view>

namespace arbiter {

// Approximate context window in tokens for a model id.  Returns 0 when
// unknown (e.g. local ollama/ models).  Prefers model_catalog entries.
int context_window_for_model(std::string_view model);

// Percentage 0–100 of window filled by prompt_tokens, or -1 when the
// window is unknown or prompt_tokens <= 0.
int context_pct_value(int prompt_tokens, std::string_view model);

} // namespace arbiter
