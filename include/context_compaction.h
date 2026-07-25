#pragma once
// Context compaction: threshold-triggered summarize of older turns for the
// model-facing request view.  Full histories_ / session JSON stay intact.

#include "api_client.h"
#include "json.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace arbiter {

struct CompactionState {
    std::string summary;       // rolling prose summary of covered turns
    size_t covered_until = 0;  // exclusive end index into histories_
    int generation = 0;
};

struct CompactionConfig {
    int    threshold_pct         = 75;
    size_t keep_messages         = 16;
    bool   enabled               = true;
    size_t char_budget_fallback  = 300'000;
    int    summary_max_tokens    = 1024;
};

// Defaults from code constants; ARBITER_COMPACT_THRESHOLD overrides pct when set.
CompactionConfig compaction_config_from_env();

[[nodiscard]] bool is_tool_results_message(const Message& m);

// Cut index so histories_[cut …] is the recent window (keep last N), snapped
// so the kept tail starts on a real user turn when possible.
[[nodiscard]] size_t compute_cut_index(const std::vector<Message>& history,
                                       size_t keep_messages);

// Model-facing view: optional summary envelope + recent tail.
[[nodiscard]] std::vector<Message>
build_model_messages(const std::vector<Message>& history,
                     const CompactionState& state);

[[nodiscard]] size_t model_view_char_count(const std::vector<Message>& msgs);

// True when auto-compaction should run (threshold or char-budget fallback).
[[nodiscard]] bool should_auto_compact(const CompactionConfig& cfg,
                                       int last_input_tokens,
                                       const std::string& model,
                                       size_t history_len,
                                       size_t model_view_chars,
                                       bool already_compacted_this_turn);

// Resolve summarize model: advisor.model when set, else executor_model.
[[nodiscard]] std::string resolve_summarize_model(
    const std::string& advisor_model,
    const std::string& executor_model);

// One-shot history-less summarize.  Empty string on failure.
[[nodiscard]] std::string summarize_history_slice(
    ApiClient& client,
    const std::string& model,
    const std::vector<Message>& older_slice,
    const std::string& prior_summary,
    const std::string& pinned_facts,
    int max_tokens = 1024);

// Attempt compaction.  On success updates state and returns true.
// Fail-open: leaves state unchanged and returns false on error / nothing to do.
bool run_compaction(ApiClient& client,
                    const std::string& summarize_model,
                    const std::vector<Message>& history,
                    CompactionState& state,
                    const CompactionConfig& cfg,
                    const std::string& pinned_facts = {});

// If covered_until is past history_len, clear state; otherwise no-op.
void sanitize_compaction_state(CompactionState& state, size_t history_len);

// Session JSON helpers (version 2 compaction map values).
[[nodiscard]] std::shared_ptr<JsonValue> compaction_to_json(const CompactionState& s);
[[nodiscard]] CompactionState compaction_from_json(const JsonValue* v);

} // namespace arbiter
