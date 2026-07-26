#pragma once
// arbiter/include/agent.h — Individual agent with conversation history + constitution

#include "constitution.h"
#include "api_client.h"
#include "agent_conversation.h"
#include "context_compaction.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <chrono>

namespace arbiter {

struct AgentStats {
    // Atomic: written by the request thread mid-turn while other threads
    // (status_summary from the REPL, to_json from the autosave worker)
    // read concurrently.
    std::atomic<int> total_input_tokens{0};
    std::atomic<int> total_output_tokens{0};
    std::atomic<int> total_requests{0};
    std::chrono::steady_clock::time_point created;   // set once in the ctor
};

class Agent {
public:
    Agent(const std::string& id, Constitution config, ApiClient& client);

    // Send a message and get response (blocking).
    ApiResponse send(const std::string& user_message);
    // Multipart variant.  Used by vision input and by tool-result re-entry
    // when one or more tool results returned image content.  The text-only
    // overload above wraps in a single text part and calls this one.
    ApiResponse send(std::vector<ContentPart> parts);

    // Send with streaming — chunks delivered via callback as they arrive.
    ApiResponse stream(const std::string& user_message, StreamCallback cb);
    ApiResponse stream(std::vector<ContentPart> parts, StreamCallback cb);

    // Append a user message to the current ConversationScope without calling
    // the model.  Used to commit tool-result envelopes (and similar synthetic
    // user turns) into history before the next LLM wait so quit/cancel/kill
    // cannot drop completed tool work.  Pair with send_continue/stream_continue
    // so the follow-up model call does not double-append the same user turn.
    void commit_user_message(const std::string& text);
    void commit_user_message(std::vector<ContentPart> parts);

    // Model call using whatever is already in history (no new user push).
    ApiResponse send_continue();
    ApiResponse stream_continue(StreamCallback cb);
    // Clear the current ConversationScope's history (keep constitution).
    void reset_history();
    // Drop every conversation slot (used when tearing down an agent).
    void reset_all_histories();
    // Drop one conversation's history without touching others.
    void erase_conversation(const std::string& conversation_id);
    // Replace history for the current ConversationScope (session restore).
    void set_history(std::vector<Message> h) {
        std::lock_guard<std::mutex> lk(history_mu_);
        const std::string key = agent_conversation_key();
        histories_[key] = std::move(h);
        // History replace invalidates index-based compaction; caller may
        // restore CompactionState afterwards (session load / API hydrate).
        compaction_[key] = CompactionState{};
        last_input_tokens_[key] = 0;
        pinned_facts_.erase(key);
        compaction_notice_.erase(key);
    }

    // Append a finished tool row onto the most recent assistant message in
    // the current ConversationScope (no-op if none).  Used so transcript
    // replay can rebuild ToolSegment chrome without re-running tools.
    void append_tool_trace(ToolTraceEntry entry);

    // Append reasoning text onto the most recent assistant message (no-op
    // when the latest message isn't assistant — e.g. mid-stream before the
    // new turn is committed).  Used so nested /agent and /parallel thought
    // deltas persist onto the pane agent's turn for conversation switch.
    void append_thinking(std::string_view delta);

    // Accessors
    const std::string& id() const { return id_; }
    const Constitution& config() const { return config_; }
    Constitution& config_mut() { return config_; }
    const AgentStats& stats() const { return stats_; }
    // Returns a copy, not a reference: a background save (ConversationStore's
    // autosave thread) reads this concurrently with a pane's exec thread
    // appending to history mid-turn, so callers must not hold a reference
    // into live state.  Reads the current ConversationScope's slot.
    std::vector<Message> history() const {
        std::lock_guard<std::mutex> lk(history_mu_);
        auto it = histories_.find(agent_conversation_key());
        if (it == histories_.end()) return {};
        return it->second;
    }
    // True if any messages are stored under `conversation_id`.
    [[nodiscard]] bool has_conversation(const std::string& conversation_id) const {
        std::lock_guard<std::mutex> lk(history_mu_);
        auto it = histories_.find(conversation_id);
        return it != histories_.end() && !it->second.empty();
    }

    // Compaction state for the current ConversationScope (full history is
    // never trimmed — this only affects the model-facing request view).
    CompactionState compaction_state() const {
        std::lock_guard<std::mutex> lk(history_mu_);
        auto it = compaction_.find(agent_conversation_key());
        if (it == compaction_.end()) return {};
        return it->second;
    }
    void set_compaction_state(CompactionState state) {
        std::lock_guard<std::mutex> lk(history_mu_);
        const std::string key = agent_conversation_key();
        sanitize_compaction_state(state, histories_[key].size());
        compaction_[key] = std::move(state);
    }

    // Force compaction for the current scope (TUI /compact).  Returns true
    // when the state advanced.  Fail-open on summarize errors.
    bool force_compact(const std::string& pinned_facts = {});

    // Pop a one-shot UX note set when compaction succeeds for the current
    // ConversationScope (empty if none).  Scoped so concurrent panes cannot
    // steal each other's notices.
    std::string take_compaction_notice() {
        std::lock_guard<std::mutex> lk(history_mu_);
        const std::string key = agent_conversation_key();
        auto it = compaction_notice_.find(key);
        if (it == compaction_notice_.end()) return {};
        std::string n = std::move(it->second);
        compaction_notice_.erase(it);
        return n;
    }

    // Optional pinned facts (open todos, etc.) for the current ConversationScope,
    // merged into summarize prompts.  Scoped so pane/thread switches do not leak.
    void set_compaction_pinned_facts(std::string facts) {
        std::lock_guard<std::mutex> lk(history_mu_);
        pinned_facts_[agent_conversation_key()] = std::move(facts);
    }
    std::string compaction_pinned_facts() const {
        std::lock_guard<std::mutex> lk(history_mu_);
        auto it = pinned_facts_.find(agent_conversation_key());
        if (it == pinned_facts_.end()) return {};
        return it->second;
    }

    // Compaction knobs (threshold / keep window) for remap helpers.
    const CompactionConfig& compaction_config() const { return compact_cfg_; }

    std::string status_summary() const;

    std::string to_json() const;

private:
    std::string id_;
    Constitution config_;
    ApiClient& client_;
    mutable std::mutex history_mu_;
    // Histories keyed by ConversationScope id ("" outside a scope).
    std::unordered_map<std::string, std::vector<Message>> histories_;
    std::unordered_map<std::string, CompactionState> compaction_;
    std::unordered_map<std::string, int> last_input_tokens_;
    std::unordered_map<std::string, std::string> pinned_facts_;
    std::unordered_map<std::string, std::string> compaction_notice_;
    CompactionConfig compact_cfg_ = compaction_config_from_env();
    AgentStats stats_;

    // Build provider messages under history_mu_ (caller must hold the lock).
    std::vector<Message> model_messages_locked(const std::string& key) const;

    // Maybe run auto-compaction.  Must NOT be called while holding history_mu_.
    // Returns true if compaction advanced.
    bool maybe_compact(bool force);

    // Concat continuation turns onto `resp` until the model actually finishes
    // (stop_reason != "max_tokens") or a cap is hit.  Pushes partial assistant
    // + "continue" prompts into the scoped history during the loop and pops
    // them before returning so the caller can commit a single merged
    // assistant turn.  `cb` may be null — null triggers the blocking
    // client_.complete() path, non-null triggers client_.stream() so
    // additional chunks flow through.
    void continue_until_done(ApiResponse& resp, StreamCallback cb);
};

} // namespace arbiter
