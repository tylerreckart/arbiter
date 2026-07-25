#include "context_compaction.h"

#include "model_context.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string_view>

namespace arbiter {

CompactionConfig compaction_config_from_env() {
    CompactionConfig cfg;
    if (const char* env = std::getenv("ARBITER_COMPACT_THRESHOLD");
        env && *env) {
        char* end = nullptr;
        long v = std::strtol(env, &end, 10);
        if (end != env && v > 0 && v <= 100)
            cfg.threshold_pct = static_cast<int>(v);
    }
    if (const char* env = std::getenv("ARBITER_COMPACT_DISABLED");
        env && (*env == '1' || *env == 't' || *env == 'T' || *env == 'y' ||
                *env == 'Y')) {
        cfg.enabled = false;
    }
    return cfg;
}

bool is_tool_results_message(const Message& m) {
    if (m.role != "user") return false;
    static constexpr std::string_view kPrefix = "[TOOL RESULTS]";
    return m.content.size() >= kPrefix.size() &&
           m.content.compare(0, kPrefix.size(), kPrefix) == 0;
}

size_t compute_cut_index(const std::vector<Message>& history,
                         size_t keep_messages) {
    if (keep_messages == 0 || history.size() <= keep_messages) return 0;
    size_t cut = history.size() - keep_messages;

    auto is_good_start = [&](size_t i) -> bool {
        if (i >= history.size()) return false;
        if (history[i].role != "user") return false;
        if (is_tool_results_message(history[i])) return false;
        return true;
    };

    if (is_good_start(cut)) return cut;

    // Walk back so the kept tail starts on a real user turn (not a tool
    // re-entry frame) when the raw cut would orphan mid-turn.
    for (size_t i = cut; i > 0;) {
        --i;
        if (is_good_start(i)) return i;
    }
    return cut;
}

std::vector<Message>
build_model_messages(const std::vector<Message>& history,
                     const CompactionState& state) {
    CompactionState s = state;
    sanitize_compaction_state(s, history.size());

    std::vector<Message> out;
    if (!s.summary.empty()) {
        out.push_back(Message{
            "user",
            "[CONVERSATION SUMMARY]\n" + s.summary + "\n[END SUMMARY]"});
        out.push_back(Message{
            "assistant",
            "Understood — continuing from the summary."});
    }

    const size_t start = std::min(s.covered_until, history.size());
    for (size_t i = start; i < history.size(); ++i)
        out.push_back(history[i]);
    return out;
}

size_t model_view_char_count(const std::vector<Message>& msgs) {
    size_t n = 0;
    for (const auto& m : msgs) n += m.content.size();
    return n;
}

bool should_auto_compact(const CompactionConfig& cfg,
                         int last_input_tokens,
                         const std::string& model,
                         size_t history_len,
                         size_t model_view_chars,
                         bool already_compacted_this_turn) {
    if (!cfg.enabled) return false;
    if (already_compacted_this_turn) return false;
    if (history_len <= cfg.keep_messages) return false;

    const int pct = context_pct_value(last_input_tokens, model);
    if (pct >= 0) return pct >= cfg.threshold_pct;

    return model_view_chars >= cfg.char_budget_fallback;
}

std::string resolve_summarize_model(const std::string& advisor_model,
                                    const std::string& executor_model) {
    if (!advisor_model.empty()) return advisor_model;
    return executor_model;
}

std::string summarize_history_slice(
    ApiClient& client,
    const std::string& model,
    const std::vector<Message>& older_slice,
    const std::string& prior_summary,
    const std::string& pinned_facts,
    int max_tokens) {
    if (model.empty() || older_slice.empty()) return {};

    std::ostringstream body;
    body << "Summarize the following conversation turns for an AI coding "
            "agent.  Produce a compact rolling summary that preserves:\n"
            "  - decisions made and their rationale\n"
            "  - file paths, symbols, and commands touched\n"
            "  - constraints, requirements, and user preferences\n"
            "  - unresolved questions and next steps\n"
            "Do not invent facts.  Prefer concrete names over vague "
            "restatement.  Output plain prose (no markdown fences).\n\n";

    if (!prior_summary.empty()) {
        body << "[PRIOR SUMMARY]\n" << prior_summary
             << "\n[END PRIOR SUMMARY]\n\n";
    }
    if (!pinned_facts.empty()) {
        body << "[PINNED FACTS]\n" << pinned_facts
             << "\n[END PINNED FACTS]\n\n";
    }

    body << "[MESSAGES TO SUMMARIZE]\n";
    for (const auto& m : older_slice) {
        body << m.role << ": " << m.content << "\n---\n";
    }
    body << "[END MESSAGES]\n";

    ApiRequest req;
    req.model               = model;
    req.max_tokens          = max_tokens > 0 ? max_tokens : 1024;
    req.include_temperature = false;
    req.system_prompt =
        "You compress conversation history for later model turns.  "
        "Be faithful and dense.";
    req.messages = {{"user", body.str()}};

    ApiResponse resp = client.complete(req);
    if (!resp.ok || resp.content.empty()) return {};
    return resp.content;
}

bool run_compaction(ApiClient& client,
                    const std::string& summarize_model,
                    const std::vector<Message>& history,
                    CompactionState& state,
                    const CompactionConfig& cfg,
                    const std::string& pinned_facts) {
    sanitize_compaction_state(state, history.size());

    const size_t cut = compute_cut_index(history, cfg.keep_messages);
    if (cut == 0 || cut <= state.covered_until) return false;
    if (summarize_model.empty()) return false;

    std::vector<Message> slice(
        history.begin() + static_cast<std::ptrdiff_t>(state.covered_until),
        history.begin() + static_cast<std::ptrdiff_t>(cut));

    std::string summary = summarize_history_slice(
        client, summarize_model, slice, state.summary, pinned_facts,
        cfg.summary_max_tokens);

    if (summary.empty()) {
        std::fprintf(stderr,
                     "WARN: context compaction summarize failed "
                     "(model=%s, slice=%zu msgs) — continuing without "
                     "compaction\n",
                     summarize_model.c_str(), slice.size());
        return false;
    }

    state.summary       = std::move(summary);
    state.covered_until = cut;
    ++state.generation;
    return true;
}

void sanitize_compaction_state(CompactionState& state, size_t history_len) {
    if (state.covered_until > history_len) {
        state = CompactionState{};
    }
}

std::shared_ptr<JsonValue> compaction_to_json(const CompactionState& s) {
    auto obj = jobj();
    auto& m = obj->as_object_mut();
    m["summary"]       = jstr(s.summary);
    m["covered_until"] = jnum(static_cast<double>(s.covered_until));
    m["generation"]    = jnum(static_cast<double>(s.generation));
    return obj;
}

CompactionState compaction_from_json(const JsonValue* v) {
    CompactionState s;
    if (!v || !v->is_object()) return s;
    s.summary = v->get_string("summary");
    const double cu = v->get_number("covered_until", 0.0);
    s.covered_until = cu < 0 ? 0 : static_cast<size_t>(cu);
    s.generation = v->get_int("generation", 0);
    return s;
}

} // namespace arbiter
