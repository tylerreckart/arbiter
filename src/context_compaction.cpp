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
    // re-entry frame) when the raw cut would orphan mid-turn.  If no safe
    // start exists, refuse the cut (0) so callers skip compaction rather
    // than emit a model view with broken role alternation after the
    // summary envelope.
    for (size_t i = cut; i > 0;) {
        --i;
        if (is_good_start(i)) return i;
    }
    return 0;
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
                         size_t model_view_chars) {
    if (!cfg.enabled) return false;
    if (history_len <= cfg.keep_messages) return false;

    const int pct = context_pct_value(last_input_tokens, model);
    if (pct >= 0) return pct >= cfg.threshold_pct;

    return model_view_chars >= cfg.char_budget_fallback;
}

std::string compaction_boundary_content(std::string_view content) {
    if (content.size() <= kCompactionBoundaryMax)
        return std::string(content);
    return std::string(content.substr(0, kCompactionBoundaryMax));
}

std::string strip_compaction_preambles(std::string_view content) {
    std::string s(content);
    // Drop [OPEN TODOS]…[END OPEN TODOS] blocks injected before the turn.
    for (;;) {
        const auto start = s.find("[OPEN TODOS]");
        if (start == std::string::npos) break;
        const auto end = s.find("[END OPEN TODOS]", start);
        if (end == std::string::npos) break;
        size_t erase_to = end + std::string_view("[END OPEN TODOS]").size();
        while (erase_to < s.size() &&
               (s[erase_to] == '\n' || s[erase_to] == '\r'))
            ++erase_to;
        s.erase(start, erase_to - start);
    }
    // Index master wraps the user line as "…\n\nQUERY: <text>".
    static constexpr std::string_view kQuery = "\n\nQUERY: ";
    const auto q = s.rfind(kQuery);
    if (q != std::string::npos)
        s = s.substr(q + kQuery.size());
    // Known-pitfalls / lesson blocks end before the real user text; if a
    // double newline remains after a leading bracket block, keep the tail.
    if (!s.empty() && s.front() == '[') {
        const auto sep = s.find("\n\n");
        if (sep != std::string::npos) s = s.substr(sep + 2);
    }
    return s;
}

bool boundary_content_matches(std::string_view db_content,
                              std::string_view boundary_content) {
    const std::string db = compaction_boundary_content(db_content);
    const std::string boundary(boundary_content);
    if (db == boundary) return true;
    // In-memory boundary may still carry a preamble the DB row lacks.
    if (boundary.size() > db.size() &&
        boundary.compare(boundary.size() - db.size(), db.size(), db) == 0)
        return true;
    return false;
}

size_t find_compaction_boundary(const std::vector<Message>& history,
                                const CompactionState& state,
                                const std::vector<int64_t>* message_db_ids) {
    // Prefer durable DB id — immune to "yes"/"continue" content collisions.
    if (message_db_ids && state.boundary_db_id > 0 &&
        message_db_ids->size() == history.size()) {
        for (size_t i = 0; i < message_db_ids->size(); ++i) {
            if ((*message_db_ids)[i] == state.boundary_db_id) return i;
        }
        return kCompactionBoundaryNpos;
    }

    if (state.boundary_role.empty() && state.boundary_content.empty())
        return kCompactionBoundaryNpos;

    // Content match only when unambiguous.
    size_t found = kCompactionBoundaryNpos;
    for (size_t i = 0; i < history.size(); ++i) {
        if (history[i].role != state.boundary_role) continue;
        if (!boundary_content_matches(history[i].content,
                                      state.boundary_content)) {
            continue;
        }
        if (found != kCompactionBoundaryNpos)
            return kCompactionBoundaryNpos;  // ambiguous
        found = i;
    }
    return found;
}

BoundaryResolve resolve_boundary_db_id(const std::vector<std::string>& roles,
                                       const std::vector<std::string>& contents,
                                       const std::vector<int64_t>& ids,
                                       const CompactionState& state) {
    BoundaryResolve out;
    if (roles.size() != contents.size() || roles.size() != ids.size())
        return out;
    if (state.boundary_role.empty() && state.boundary_content.empty())
        return out;

    std::vector<size_t> matches;
    for (size_t i = 0; i < roles.size(); ++i) {
        if (roles[i] != state.boundary_role) continue;
        if (!boundary_content_matches(contents[i], state.boundary_content))
            continue;
        matches.push_back(i);
    }
    if (matches.empty()) return out;
    if (matches.size() != 1) {
        out.status = BoundaryResolve::Status::Ambiguous;
        return out;
    }
    out.status = BoundaryResolve::Status::Unique;
    out.id = ids[matches[0]];
    return out;
}

void remap_compaction_onto_history(CompactionState& state,
                                   const std::vector<Message>& history,
                                   size_t /*keep_messages*/,
                                   const std::vector<int64_t>* message_db_ids) {
    if (state.summary.empty()) {
        state = CompactionState{};
        return;
    }

    const size_t boundary =
        find_compaction_boundary(history, state, message_db_ids);
    if (boundary == kCompactionBoundaryNpos) {
        // Missing / ambiguous / fell off replay cap.  Caller should fold
        // any DB gap into the summary before relying on covered_until=0.
        state.covered_until = 0;
    } else {
        state.covered_until = boundary;
    }
    sanitize_compaction_state(state, history.size());
}

bool fold_compaction_gap(ApiClient& client,
                         const std::string& summarize_model,
                         const std::vector<Message>& gap,
                         CompactionState& state,
                         const CompactionConfig& cfg,
                         const std::string& pinned_facts) {
    if (gap.empty() || summarize_model.empty() || state.summary.empty())
        return false;

    std::string summary = summarize_history_slice(
        client, summarize_model, gap, state.summary, pinned_facts,
        cfg.summary_max_tokens);
    if (summary.empty()) {
        std::fprintf(stderr,
                     "WARN: compaction gap fold failed (model=%s, gap=%zu "
                     "msgs) — gap may be missing from model context\n",
                     summarize_model.c_str(), gap.size());
        return false;
    }

    state.summary = std::move(summary);
    state.covered_until = 0;
    ++state.generation;
    // Boundary advances to the start of the hydrated tail (caller sets
    // boundary_role/content/db_id from prior.front() after a successful fold).
    return true;
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

    state.summary          = std::move(summary);
    state.covered_until    = cut;
    state.boundary_role    = history[cut].role;
    // Strip orchestrator preambles so the boundary matches raw DB rows.
    state.boundary_content = compaction_boundary_content(
        strip_compaction_preambles(history[cut].content));
    // Invalidate any prior DB id — it referred to the previous cut. Persist
    // must re-resolve against the new boundary text.
    state.boundary_db_id = 0;
    ++state.generation;
    return true;
}

void sanitize_compaction_state(CompactionState& state, size_t history_len) {
    if (state.covered_until > history_len) {
        state = CompactionState{};
        return;
    }
    // A covered_until without a summary would silently drop history from
    // the model view with nothing to replace it — treat as corrupt.
    if (state.summary.empty() && state.covered_until > 0) {
        state = CompactionState{};
    }
}

std::shared_ptr<JsonValue> compaction_to_json(const CompactionState& s) {
    auto obj = jobj();
    auto& m = obj->as_object_mut();
    m["summary"]       = jstr(s.summary);
    m["covered_until"] = jnum(static_cast<double>(s.covered_until));
    m["generation"]    = jnum(static_cast<double>(s.generation));
    if (!s.boundary_role.empty() || !s.boundary_content.empty()) {
        m["boundary_role"]    = jstr(s.boundary_role);
        m["boundary_content"] = jstr(s.boundary_content);
    }
    if (s.boundary_db_id > 0)
        m["boundary_db_id"] = jnum(static_cast<double>(s.boundary_db_id));
    return obj;
}

CompactionState compaction_from_json(const JsonValue* v) {
    CompactionState s;
    if (!v || !v->is_object()) return s;
    s.summary = v->get_string("summary");
    const double cu = v->get_number("covered_until", 0.0);
    s.covered_until = cu < 0 ? 0 : static_cast<size_t>(cu);
    s.generation = v->get_int("generation", 0);
    s.boundary_role = v->get_string("boundary_role");
    s.boundary_content = v->get_string("boundary_content");
    const double bid = v->get_number("boundary_db_id", 0.0);
    s.boundary_db_id = bid < 0 ? 0 : static_cast<int64_t>(bid);
    return s;
}

} // namespace arbiter
