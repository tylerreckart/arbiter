#pragma once
// Context compaction: threshold-triggered summarize of older turns for the
// model-facing request view.  Full histories_ / session JSON stay intact.

#include "api_client.h"
#include "json.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter {

struct CompactionState {
    std::string summary;       // rolling prose summary of covered turns
    size_t covered_until = 0;  // exclusive end index into histories_
    int generation = 0;
    // First kept message at compact time (histories_[covered_until]).
    // Portable across API hydrate where absolute indices are meaningless:
    // remap finds this message in the new history and restores the cut.
    std::string boundary_role;
    std::string boundary_content;  // truncated; see kCompactionBoundaryMax
    // DB messages.id of the boundary when known (HTTP conversations).
    // Prefer this over content matching — content can collide on short
    // repeats like "yes" / "continue".
    int64_t boundary_db_id = 0;
};

// Max chars of boundary_content persisted / matched.
inline constexpr size_t kCompactionBoundaryMax = 4096;
inline constexpr size_t kCompactionBoundaryNpos =
    std::numeric_limits<size_t>::max();

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
// Callers should lower last_input_tokens after a successful compact so this
// does not immediately re-fire; tool-loop re-entries may compact again once
// the live prompt tokens climb back over the threshold.
[[nodiscard]] bool should_auto_compact(const CompactionConfig& cfg,
                                       int last_input_tokens,
                                       const std::string& model,
                                       size_t history_len,
                                       size_t model_view_chars);

// Remap a persisted CompactionState onto a freshly replaced history
// (API hydrate after set_history).  Prefer boundary_db_id when
// message_db_ids is provided; otherwise require a unique role+content
// match.  Ambiguous or missing boundary → covered_until = 0 (caller should
// fold any DB gap into the summary before relying on that fallback).
void remap_compaction_onto_history(
    CompactionState& state,
    const std::vector<Message>& history,
    size_t keep_messages,
    const std::vector<int64_t>* message_db_ids = nullptr);

// Truncate message content for boundary persistence / matching.
[[nodiscard]] std::string compaction_boundary_content(std::string_view content);

// Index of the boundary message in history, or kCompactionBoundaryNpos.
// If message_db_ids is non-null and state.boundary_db_id > 0, match by id.
// Otherwise match role+content only when exactly one history message matches
// (refuse ambiguous short-message collisions).
[[nodiscard]] size_t find_compaction_boundary(
    const std::vector<Message>& history,
    const CompactionState& state,
    const std::vector<int64_t>* message_db_ids = nullptr);

// Result of resolving a boundary against DB rows.
struct BoundaryResolve {
    enum class Status { None, Unique, Ambiguous } status = Status::None;
    int64_t id = 0;
};

// Resolve boundary_db_id from a list of (role, content, id) DB rows.
// Unique → Status::Unique; several content matches → Ambiguous; else None.
// Content match allows DB text to be a suffix of the in-memory boundary
// (orchestrator preambles are not persisted to SQLite).
[[nodiscard]] BoundaryResolve resolve_boundary_db_id(
    const std::vector<std::string>& roles,
    const std::vector<std::string>& contents,
    const std::vector<int64_t>& ids,
    const CompactionState& state);

// Strip orchestrator-injected preambles from a user message so the stored
// boundary matches raw DB conversation rows.
[[nodiscard]] std::string strip_compaction_preambles(std::string_view content);

// True if db_content matches boundary_content (exact truncated equality, or
// db_content is a suffix of the in-memory boundary after preamble strip).
[[nodiscard]] bool boundary_content_matches(std::string_view db_content,
                                            std::string_view boundary_content);

// Fold gap messages (boundary..start_of_hydrated_tail) into the rolling
// summary.  On success updates summary/generation/boundary fields and
// sets covered_until = 0.  Fail-open: leaves state unchanged, returns false.
bool fold_compaction_gap(ApiClient& client,
                         const std::string& summarize_model,
                         const std::vector<Message>& gap,
                         CompactionState& state,
                         const CompactionConfig& cfg,
                         const std::string& pinned_facts = {});

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
